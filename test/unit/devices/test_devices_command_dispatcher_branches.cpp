#include <cstdint>
#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/command_dispatcher.h"
#include "jandy/devices/iaq_device.h"
#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"

#include "interfaces/icommanddispatcher.h"

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Interfaces;
using namespace AqualinkAutomate::Kernel;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

//=============================================================================
// CommandDispatcher -- the capability-routing arms the main suite does not
// reach: the controller-schedule DELETE / EDIT commands, the InvalidValue and
// Busy outcomes of the priority walk (and the command-history record that goes
// with them), and the chlorinator's body validation + spa write-through.
//=============================================================================

namespace
{
	using Result = ICommandDispatcher::CommandResult;

	struct DispatcherBranchesFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		DispatcherBranchesFixture()
			: data_hub(Find<DataHub>())
			, equipment_hub(Find<EquipmentHub>())
			, dispatcher(data_hub, equipment_hub)
		{
		}

		IAQDevice& AddIAQ(bool emulated)
		{
			auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(0x33));
			auto device = std::make_unique<IAQDevice>(id, *this, emulated);
			IAQDevice& ref = *device;
			equipment_hub->AddDevice(std::move(device));
			return ref;
		}

		void AddOneTouch(bool emulated)
		{
			auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(0x41));
			equipment_hub->AddDevice(std::make_unique<OneTouchDevice>(id, *this, emulated));
		}

		void AddChlorinator()
		{
			auto chlorinator = std::make_shared<AuxillaryDevice>();
			chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
			chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
			data_hub->Devices.Add(std::move(chlorinator));
		}

		std::shared_ptr<DataHub> data_hub;
		std::shared_ptr<EquipmentHub> equipment_hub;
		CommandDispatcher dispatcher;
	};

	Scheduling::ControllerSchedule MakeProgram(const std::string& target, std::uint8_t days = 0x7f)
	{
		Scheduling::ControllerSchedule program;
		program.target = target;
		program.days_of_week = days;
		program.on_hour = 9;
		program.on_minute = 0;
		program.off_hour = 17;
		program.off_minute = 0;
		return program;
	}
}
// unnamed namespace

BOOST_FIXTURE_TEST_SUITE(CommandDispatcherBranches_TestSuite, DispatcherBranchesFixture)

//-----------------------------------------------------------------------------
// Controller-schedule delete / edit
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ControllerProgramWrites_WithNoCapableController_AreRefusedHonestly)
{
	// Nothing on the bus can write the controller's own program list: the caller must be told
	// so, not given a silent success.
	const auto program = MakeProgram("Pool Light");

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.DeleteControllerProgram(program)), static_cast<int>(Result::NoSerialAdapter));
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.EditControllerProgram(program, program)), static_cast<int>(Result::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(ControllerProgramWrites_WithOnlyAPassiveController_AreRefusedHonestly)
{
	// A passive (non-emulated) panel advertises the capability but can never transmit, so it
	// reports NotSupported and the walk ends with the honest "nothing can do this" result.
	AddIAQ(/*emulated=*/false);
	const auto program = MakeProgram("Pool Light");

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.DeleteControllerProgram(program)), static_cast<int>(Result::NoSerialAdapter));
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.EditControllerProgram(program, program)), static_cast<int>(Result::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(DeleteControllerProgram_EmulatedIAQ_IsQueuedAndRecorded)
{
	auto& iaq = AddIAQ(/*emulated=*/true);

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.DeleteControllerProgram(MakeProgram("Pool Light"))), static_cast<int>(Result::Success));

	// A dispatched command is recorded on the controller that handled it, for the device card's
	// "Recent Commands" list.
	const auto diagnostics = iaq.DescribeDiagnostics();
	BOOST_REQUIRE_EQUAL(diagnostics["recent_commands"].size(), 1u);
	BOOST_CHECK_EQUAL(diagnostics["recent_commands"][0]["outcome"].get<std::string>(), std::string("Success"));
	BOOST_CHECK(diagnostics["recent_commands"][0]["description"].get<std::string>().find("delete") != std::string::npos);
	BOOST_CHECK(diagnostics["recent_commands"][0]["description"].get<std::string>().find("Pool Light") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(EditControllerProgram_EmulatedIAQ_IsQueued)
{
	AddIAQ(/*emulated=*/true);

	const auto existing = MakeProgram("Pool Light", 0x7f);
	const auto desired = MakeProgram("Pool Light", 0x60);   // weekends -- a selection the panel can pick

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.EditControllerProgram(existing, desired)), static_cast<int>(Result::Success));
}

BOOST_AUTO_TEST_CASE(EditControllerProgram_ValueTheControllerCannotRepresent_IsInvalidValue)
{
	// InvalidValue STOPS the priority walk: no other controller would accept the value either,
	// so trying them would only waste time and muddy the result. It is also recorded as the
	// outcome on the controller that rejected it.
	auto& iaq = AddIAQ(/*emulated=*/true);
	AddOneTouch(/*emulated=*/true);

	const auto existing = MakeProgram("Pool Light", 0x7f);
	const auto desired = MakeProgram("Pool Light", 0x15);   // Mon+Wed+Fri -- not expressible

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.EditControllerProgram(existing, desired)), static_cast<int>(Result::InvalidValue));

	const auto diagnostics = iaq.DescribeDiagnostics();
	BOOST_REQUIRE_EQUAL(diagnostics["recent_commands"].size(), 1u);
	BOOST_CHECK_EQUAL(diagnostics["recent_commands"][0]["outcome"].get<std::string>(), std::string("InvalidValue"));
}

BOOST_AUTO_TEST_CASE(AControllerProgramWriteWhileAnotherIsInFlight_IsBusyNotUnavailable)
{
	// The shared panel UI takes one write goal at a time. A second request that lands mid-walk
	// must read as Busy (retry shortly) rather than "no capable controller" -- the difference
	// between a 409 and a 503 for the caller.
	AddIAQ(/*emulated=*/true);

	BOOST_REQUIRE_EQUAL(static_cast<int>(dispatcher.CreateControllerProgram(MakeProgram("Pool Light"))), static_cast<int>(Result::Success));
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.DeleteControllerProgram(MakeProgram("Filter Pump"))), static_cast<int>(Result::Busy));
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.EditControllerProgram(MakeProgram("Filter Pump"), MakeProgram("Filter Pump", 0x60))), static_cast<int>(Result::Busy));
}

//-----------------------------------------------------------------------------
// Chlorinator body validation + write-through
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(SetChlorinatorPercentage_ForANonAddressableBody_IsInvalidValue)
{
	// Pool and spa carry INDEPENDENT setpoints on the panel; "Shared"/"Unknown" would force the
	// dispatcher to guess a row, which is exactly what the body parameter exists to prevent.
	AddOneTouch(/*emulated=*/true);

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.SetChlorinatorPercentage(50, BodyOfWaterIds::Shared)), static_cast<int>(Result::InvalidValue));
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.SetChlorinatorPercentage(50, BodyOfWaterIds::Unknown)), static_cast<int>(Result::InvalidValue));
}

BOOST_AUTO_TEST_CASE(SetChlorinatorPercentage_Spa_WritesThroughToTheSpaSetpointOnly)
{
	// The optimistic write-through must land on the body that was asked for: writing the pool
	// trait for a spa request would show the user a value they never set.
	AddChlorinator();
	AddOneTouch(/*emulated=*/true);

	BOOST_REQUIRE_EQUAL(static_cast<int>(dispatcher.SetChlorinatorPercentage(60, BodyOfWaterIds::Spa)), static_cast<int>(Result::Success));

	auto chlorinators = data_hub->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);
	auto spa = chlorinators.front()->AuxillaryTraits.TryGet(ChlorinatorSpaSetpointTrait{});
	BOOST_REQUIRE(spa.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(spa.value()), 60);
	BOOST_CHECK(!chlorinators.front()->AuxillaryTraits.TryGet(ChlorinatorPoolSetpointTrait{}).has_value());
}

BOOST_AUTO_TEST_CASE(SetChlorinatorPercentage_WithNoChlorinatorDiscovered_StillDispatches)
{
	// The write-through is a cache convenience, not a precondition: with no chlorinator device
	// in the hub yet the command must still be queued on the wire.
	AddOneTouch(/*emulated=*/true);

	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher.SetChlorinatorPercentage(60, BodyOfWaterIds::Spa)), static_cast<int>(Result::Success));
	BOOST_CHECK(data_hub->Chlorinators().empty());
}

BOOST_AUTO_TEST_SUITE_END()
