#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"

#include "kernel/data_hub.h"
#include "kernel/preferences_hub.h"
#include "kernel/system_boards.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/onetouch_test_device.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// OneTouchDevice lifecycle / capability edges not reached by the core suite:
// the diagnostics "clear & rediscover" action, the explicit On/Off no-op paths of
// ActuateDevice, the busy / invalid-value refusals of the chlorinator and
// spa-switch capabilities, the proactive chlorinator-setpoint refresh crawl, and
// the watchdog-timeout op-state transitions.
//=============================================================================

namespace
{
	using TestDevice = Test::SeamedOneTouchDevice;
	namespace ATT = Kernel::AuxillaryTraitsTypes;

	struct LifecycleFixture : public Test::HubLocatorInjector
	{
		LifecycleFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x40)))
			, data_hub(Find<Kernel::DataHub>())
			, preferences_hub(Find<Kernel::PreferencesHub>())
		{
		}

		// A numbered aux the way the Equipment ON/OFF scrape creates it (type + Jandy id + label).
		std::shared_ptr<Kernel::AuxillaryDevice> SeedAux(Auxillaries::JandyAuxillaryIds aux_id, const std::string& label)
		{
			auto aux = std::make_shared<Kernel::AuxillaryDevice>();
			aux->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Auxillary);
			aux->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, aux_id);
			if (!label.empty())
			{
				aux->AuxillaryTraits.Set(ATT::LabelTrait{}, label);
			}
			data_hub->Devices.Add(aux);
			return aux;
		}

		static std::string OperatingStateOf(const OneTouchDevice& device)
		{
			return device.DescribeDiagnostics().at("operating_state").get<std::string>();
		}

		static void RenderSystemPage(TestDevice& device)
		{
			device.RenderScreenLineForTest(9, "Equipment ON/OFF");
		}

		// Reach NormalOperation deterministically via the fault-recovery hysteresis path.
		static void RecoverToNormalOperation(TestDevice& device)
		{
			device.ForceScrapingFaultedForTest();
			RenderSystemPage(device);
			for (uint32_t i = 0; i < 3; ++i)
			{
				device.DeliverStatusFrameForTest();
			}
			BOOST_REQUIRE(device.IsInNormalOperationForTest());
		}

		std::shared_ptr<JandyDeviceType> device_type;
		std::shared_ptr<Kernel::DataHub> data_hub;
		std::shared_ptr<Kernel::PreferencesHub> preferences_hub;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_Lifecycle, LifecycleFixture)

//=============================================================================
// IEquipmentDiscoveryController: RequestFullRediscovery / DiscoveryStatus
//=============================================================================

BOOST_AUTO_TEST_CASE(Rediscovery_NonEmulated_IsRefused)
{
	TestDevice device(device_type, *this, /*emulated*/ false);
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");

	BOOST_CHECK(!device.RequestFullRediscovery());

	const auto status = device.DiscoveryStatus();
	BOOST_CHECK(!status.in_progress);
	BOOST_CHECK_EQUAL(status.last_cleared_count, 0u);
	// Nothing was cleared on the refused path.
	BOOST_CHECK_EQUAL(data_hub->Auxillaries().size(), 1u);
}

BOOST_AUTO_TEST_CASE(Rediscovery_Emulated_ClearsAuxesResetsModelAndStartsCrawl)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_2, "");
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_3, "Waterfall");

	// Model facts a previous REV-page decode would have left behind.
	data_hub->ExpectedAuxillaryCount = 7;
	data_hub->ExpectedPowerCenterCount = 1;
	data_hub->SystemBoard = Kernel::SystemBoards::RS8_Combo;
	data_hub->EquipmentValidationResult = Kernel::EquipmentValidation{};

	BOOST_REQUIRE(device.RequestFullRediscovery());

	// Every auto-detected aux is gone and the model facts are back to Unknown.
	BOOST_CHECK(data_hub->Auxillaries().empty());
	BOOST_CHECK_EQUAL(static_cast<int>(data_hub->ExpectedAuxillaryCount), 0);
	BOOST_CHECK_EQUAL(static_cast<int>(data_hub->ExpectedPowerCenterCount), 0);
	BOOST_CHECK(data_hub->SystemBoard == Kernel::SystemBoards::Unknown);
	BOOST_CHECK(!data_hub->EquipmentValidationResult.has_value());

	const auto status = device.DiscoveryStatus();
	BOOST_CHECK(status.in_progress);
	BOOST_CHECK_EQUAL(status.last_cleared_count, 3u);

	// The spider engine is now crawling, which the diagnostics surface too.
	const auto diag = device.DescribeDiagnostics();
	BOOST_CHECK_NE(diag.at("spider_engine").at("state").get<std::string>(), "Idle");

	// A second request while the crawl is in flight is refused (and clears nothing).
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_4, "Fountain");
	BOOST_CHECK(!device.RequestFullRediscovery());
	BOOST_CHECK_EQUAL(data_hub->Auxillaries().size(), 1u);
	BOOST_CHECK_EQUAL(device.DiscoveryStatus().last_cleared_count, 3u);
}

BOOST_AUTO_TEST_CASE(Rediscovery_SparesOperatorForcedPresentAux)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_2, "Spa Light");

	// The operator has forced Aux_2 present: it is a declaration, not a detection.
	preferences_hub->AuxPresenceOverrides = nlohmann::json{ { "Aux2", "present" } };

	BOOST_REQUIRE(device.RequestFullRediscovery());
	BOOST_CHECK_EQUAL(device.DiscoveryStatus().last_cleared_count, 1u);

	auto remaining = data_hub->Auxillaries();
	BOOST_REQUIRE_EQUAL(remaining.size(), 1u);
	BOOST_CHECK(*(remaining.front()->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]) == Auxillaries::JandyAuxillaryIds::Aux_2);
}

//=============================================================================
// DeviceActuator: ActuateDevice mapping / explicit On-Off no-op paths / busy
//=============================================================================

BOOST_AUTO_TEST_CASE(Actuate_NullDevice_MappingFailed)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	BOOST_CHECK(device.ActuateDevice(nullptr, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::MappingFailed);
}

BOOST_AUTO_TEST_CASE(Actuate_DeviceWithoutLabel_MappingFailed)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	auto unlabelled = std::make_shared<Kernel::AuxillaryDevice>();
	unlabelled->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Auxillary);
	BOOST_CHECK(device.ActuateDevice(unlabelled, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::MappingFailed);

	auto blank = std::make_shared<Kernel::AuxillaryDevice>();
	blank->AuxillaryTraits.Set(ATT::LabelTrait{}, std::string{ "   " });
	BOOST_CHECK(device.ActuateDevice(blank, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::MappingFailed);

	// Nothing was queued by either refusal: the keypad is still free.
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Actuate_ExplicitOn_WhenAlreadyOn_IsNoOp)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	auto aux = SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	aux->AuxillaryTraits.Set(ATT::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);

	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	// No toggle goal was queued (a Select would have flipped it OFF): the keypad is still free.
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Actuate_ExplicitOff_WhenAlreadyOff_IsNoOp)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	auto aux = SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	aux->AuxillaryTraits.Set(ATT::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);

	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Actuate_ExplicitOn_WhenOff_QueuesToggle)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	auto aux = SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	aux->AuxillaryTraits.Set(ATT::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);

	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	// The toggle goal now holds the keypad.
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);
}

BOOST_AUTO_TEST_CASE(Actuate_ExplicitOn_UnknownState_QueuesToggle)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	// No type trait at all -> current state unknown -> act.
	auto untyped = std::make_shared<Kernel::AuxillaryDevice>();
	untyped->AuxillaryTraits.Set(ATT::LabelTrait{}, std::string{ "Pool Light" });
	BOOST_CHECK(device.ActuateDevice(untyped, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);

	// Typed aux without a status trait -> also unknown -> act.
	TestDevice other(device_type, *this, /*emulated*/ true);
	auto aux = SeedAux(Auxillaries::JandyAuxillaryIds::Aux_2, "Spa Light");
	BOOST_CHECK(other.ActuateDevice(aux, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(other.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);
}

BOOST_AUTO_TEST_CASE(Actuate_Pump_UsesPumpStatusForNoOpDecision)
{
	TestDevice running_device(device_type, *this, /*emulated*/ true);

	auto pump = std::make_shared<Kernel::AuxillaryDevice>();
	pump->AuxillaryTraits.Set(ATT::LabelTrait{}, std::string{ "Filter Pump" });
	pump->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Pump);
	pump->AuxillaryTraits.Set(ATT::PumpStatusTrait{}, Kernel::PumpStatuses::Running);

	// Already running: On is a no-op.
	BOOST_CHECK(running_device.ActuateDevice(pump, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(running_device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);

	// Running but asked Off: acts.
	TestDevice stopping_device(device_type, *this, /*emulated*/ true);
	BOOST_CHECK(stopping_device.ActuateDevice(pump, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(stopping_device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);

	// A pump with no status trait: unknown -> acts.
	TestDevice unknown_device(device_type, *this, /*emulated*/ true);
	auto silent_pump = std::make_shared<Kernel::AuxillaryDevice>();
	silent_pump->AuxillaryTraits.Set(ATT::LabelTrait{}, std::string{ "Filter Pump" });
	silent_pump->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Pump);
	BOOST_CHECK(unknown_device.ActuateDevice(silent_pump, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(unknown_device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);
}

BOOST_AUTO_TEST_CASE(Actuate_WhileGoalInFlight_IsBusy)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	auto aux = SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");

	BOOST_REQUIRE(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Busy);
}

//=============================================================================
// ChlorinatorController / SpaSwitchConfigurator refusals
//=============================================================================

BOOST_AUTO_TEST_CASE(Chlorinator_InvalidBody_IsInvalidValue)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	BOOST_CHECK(device.SetChlorinatorPercentage(50, Kernel::BodyOfWaterIds::Shared) == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK(device.SetChlorinatorPercentage(50, Kernel::BodyOfWaterIds::Unknown) == Capabilities::ActuationResult::InvalidValue);
	// Neither refusal queued anything; the spa row edit is accepted next.
	BOOST_CHECK(device.SetChlorinatorPercentage(120, Kernel::BodyOfWaterIds::Spa) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(ChlorinatorBoost_WhileGoalInFlight_IsBusy)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	BOOST_REQUIRE(device.SetChlorinatorBoost(true) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetChlorinatorBoost(false) == Capabilities::ActuationResult::Busy);
}

BOOST_AUTO_TEST_CASE(SpaSwitch_InvalidArguments_AndBusy)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	BOOST_CHECK(device.SetSpaSwitchAssignment(0, 1, "Pool Light") == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK(device.SetSpaSwitchAssignment(1, 0, "Pool Light") == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK(device.SetSpaSwitchAssignment(1, 1, "") == Capabilities::ActuationResult::InvalidValue);

	BOOST_REQUIRE(device.SetSpaSwitchAssignment(1, 2, "Pool Light") == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetSpaSwitchAssignment(1, 3, "Spa") == Capabilities::ActuationResult::Busy);
}

//=============================================================================
// Proactive chlorinator-setpoint refresh (read-only Set AquaPure re-scrape)
//=============================================================================

BOOST_AUTO_TEST_CASE(SetpointRefresh_Disabled_NeverHoldsTheKeypad)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	RecoverToNormalOperation(device);

	// Interval 0 = disabled: frames in NormalOperation never start a refresh crawl.
	device.EnableChlorinatorSetpointRefresh(std::chrono::seconds(0));
	for (int i = 0; i < 3; ++i)
	{
		device.DeliverStatusFrameForTest();
	}
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(SetpointRefresh_StartsOnFirstEligibleFrame_AndReleasesWhenCrawlEnds)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	RecoverToNormalOperation(device);

	device.EnableChlorinatorSetpointRefresh(std::chrono::seconds(1));

	// The first NormalOperation frame kicks off the read-only crawl, which counts as a goal:
	// a user command arriving mid-refresh is told to retry rather than interleave.
	device.DeliverStatusFrameForTest();
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Busy);

	// Let the spider sync on the home page (three consistent detections) and begin navigating.
	device.DeliverStatusFrameForTest();
	device.DeliverStatusFrameForTest();
	BOOST_CHECK(device.SetChlorinatorBoost(true) == Capabilities::ActuationResult::Busy);
	BOOST_CHECK(device.IsInNormalOperationForTest());

	// The controller drops into Service Mode: navigation fails, the targeted crawl ends, and
	// the refresh releases the keypad (the device stays in NormalOperation throughout).
	device.RenderScreenLineForTest(9, "");
	device.RenderScreenLineForTest(3, "  Service Mode  ");
	for (int i = 0; i < 6; ++i)
	{
		device.DeliverStatusFrameForTest();
	}
	BOOST_CHECK(device.IsInNormalOperationForTest());
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(SetpointRefresh_DeferredWhileUserGoalInFlight)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	RecoverToNormalOperation(device);

	device.EnableChlorinatorSetpointRefresh(std::chrono::seconds(1));

	// A user goal is queued first: the refresh must not start over the top of it.
	BOOST_REQUIRE(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
	device.DeliverStatusFrameForTest();

	const auto diag = device.DescribeDiagnostics();
	// The spider engine is idle: no refresh crawl was started while the user edit is in flight.
	BOOST_CHECK_EQUAL(diag.at("spider_engine").at("state").get<std::string>(), "Idle");
}

//=============================================================================
// Watchdog timeout op-state transitions
//=============================================================================

BOOST_AUTO_TEST_CASE(Watchdog_DuringScraping_DegradesToNormalOperation)
{
	TestDevice device(device_type, *this, /*emulated*/ true);

	// A recognised page moves the emulated device from ColdStart into the startup crawl.
	RenderSystemPage(device);
	device.DeliverStatusFrameForTest();
	BOOST_REQUIRE_EQUAL(OperatingStateOf(device), "Scraping");

	// The bus goes quiet: the crawl is abandoned and the device falls back to NormalOperation
	// so it stays (passively) useful and actuatable.
	device.FireWatchdogTimeoutForTest();
	BOOST_CHECK_EQUAL(OperatingStateOf(device), "NormalOperation");
	BOOST_CHECK(device.IsInNormalOperationForTest());
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Watchdog_DuringColdStart_IsAFault)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	BOOST_REQUIRE_EQUAL(OperatingStateOf(device), "ColdStart");

	// No recognisable page ever arrived: the device faults (and honestly refuses actuation).
	device.FireWatchdogTimeoutForTest();
	BOOST_CHECK_EQUAL(OperatingStateOf(device), "FaultHasOccurred");
	BOOST_CHECK(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics().at("is_running").get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(Watchdog_DuringNormalOperation_LeavesStateAlone)
{
	TestDevice device(device_type, *this, /*emulated*/ true);
	RecoverToNormalOperation(device);

	device.FireWatchdogTimeoutForTest();
	BOOST_CHECK_EQUAL(OperatingStateOf(device), "NormalOperation");
}

BOOST_AUTO_TEST_SUITE_END()
