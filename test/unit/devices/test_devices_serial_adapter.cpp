#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/serial_adapter_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/serial_adapter/serial_adapter_message_dev_status.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_status.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "kernel/pool_configurations.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

#include "kernel/system_boards.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Messages;

namespace
{
	struct SerialAdapterDeviceFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		SerialAdapterDeviceFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x48)))
		{
		}

		std::shared_ptr<JandyDeviceType> device_type;
	};
}

BOOST_FIXTURE_TEST_SUITE(SerialAdapterDevice_TestSuite, SerialAdapterDeviceFixture)

// =============================================================================
// Construction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_Emulated)
{
	BOOST_CHECK_NO_THROW(SerialAdapterDevice device(device_type, *this, true));
}

BOOST_AUTO_TEST_CASE(TestConstruction_NonEmulated)
{
	BOOST_CHECK_NO_THROW(SerialAdapterDevice device(device_type, *this, false));
}

// =============================================================================
// Destruction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDestruction_CleanAfterConstruction)
{
	{
		SerialAdapterDevice device(device_type, *this, true);
	}
	BOOST_CHECK(true);
}

// =============================================================================
// QueueCommand
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueCommand_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x05, 0x0A));
}

BOOST_AUTO_TEST_CASE(TestQueueCommand_MultipleCommands)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x05, 0x0A));
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x80, 0x10));
}

// =============================================================================
// QueuePumpCommand
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueuePumpCommand_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueuePumpCommand(SerialAdapter_SystemPumpCommands::PUMP, SerialAdapter_CommandTypes::Toggle));
}

BOOST_AUTO_TEST_CASE(TestQueuePumpCommand_SetOn)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueuePumpCommand(SerialAdapter_SystemPumpCommands::PUMP, SerialAdapter_CommandTypes::SetOn));
}

BOOST_AUTO_TEST_CASE(TestQueuePumpCommand_SetOff)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPA, SerialAdapter_CommandTypes::SetOff));
}

// =============================================================================
// SetCirculationMode (OBS-06)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetCirculationMode_PoolSpaSpillover_Accepted)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Pool) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spa) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spillover) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(TestSetCirculationMode_SpaFillDrain_NotSupported)
{
	// Spa Fill / Spa Drain have no RS-485 command (valve/service operations) -> NotSupported,
	// not InvalidValue or a silent accept.
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::SpaFill) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::SpaDrain) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(TestSetCirculationMode_SingleBody_RejectsSpaAndSpillover)
{
	// On a KNOWN single-body (pool-only/spa-only) system there is no second body to move water
	// to/from, so SPA and SPILLOVER are not available (serial-adapter host protocol, note 4).
	// Pool (spa-off) stays available.
	auto data_hub = this->Find<AqualinkAutomate::Kernel::DataHub>();
	BOOST_REQUIRE(data_hub != nullptr);
	data_hub->PoolConfiguration = AqualinkAutomate::Kernel::PoolConfigurations::SingleBody;

	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spa) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spillover) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Pool) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(TestSetCirculationMode_DualBody_AllowsSpaAndSpillover)
{
	// A combo / dual-equipment system has both bodies, so SPA and SPILLOVER are available.
	auto data_hub = this->Find<AqualinkAutomate::Kernel::DataHub>();
	BOOST_REQUIRE(data_hub != nullptr);
	data_hub->PoolConfiguration = AqualinkAutomate::Kernel::PoolConfigurations::DualBody_SharedEquipment;

	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spa) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetCirculationMode(AqualinkAutomate::Kernel::CirculationModes::Spillover) == Capabilities::ActuationResult::Accepted);
}

// =============================================================================
// SetHeaterMode (capture-gated heater enable/disable)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetHeaterMode_PoolSpaSolar_Accepted)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetHeaterMode(AqualinkAutomate::Kernel::BodyOfWaterIds::Pool, true) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetHeaterMode(AqualinkAutomate::Kernel::BodyOfWaterIds::Spa, false) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(device.SetHeaterMode(AqualinkAutomate::Kernel::BodyOfWaterIds::Shared, true) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(TestSetHeaterMode_UnknownBody_MappingFailed)
{
	// A body with no heater command (e.g. Unknown) is a well-formed request this adapter cannot map.
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.SetHeaterMode(AqualinkAutomate::Kernel::BodyOfWaterIds::Unknown, true) == Capabilities::ActuationResult::MappingFailed);
}

// =============================================================================
// QueueAuxCommand
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueAuxCommand_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueAuxCommand(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_1, SerialAdapter_CommandTypes::Toggle));
}

BOOST_AUTO_TEST_CASE(TestQueueAuxCommand_SetOn)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueAuxCommand(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_3, SerialAdapter_CommandTypes::SetOn));
}

// =============================================================================
// ActuateDevice deliverability gate (regression: auto-mode "click does nothing")
//
// An emulated adapter only transmits in response to a master poll; every poll
// Kick()s the watchdog, so IsRunning() reflects whether the master is actively
// polling our address. When the master never polls 0x48 (the RS Serial Adapter is
// an optional add-on), IsRunning() falls false and a queued command would never be
// transmitted. ActuateDevice must therefore report NotSupported (so the dispatcher
// falls back to a controller the master IS polling, e.g. an emulated OneTouch), NOT
// a false Accepted. (The positive end-to-end path -- a real setDev {state, devID}
// frame on the wire -- is covered in test_flow_command_to_wire.cpp.)
// =============================================================================

namespace
{
	// Test shim: exposes the protected watchdog Stop() so a test can simulate "the master
	// is not polling this adapter" (IsRunning() == false) without waiting out the real 30s
	// watchdog timeout.
	struct StoppableSerialAdapterDevice : public SerialAdapterDevice
	{
		using SerialAdapterDevice::SerialAdapterDevice;
		void SimulateNotPolled() { Stop(); }
	};
}

BOOST_AUTO_TEST_CASE(TestActuateDevice_NotRunning_ReturnsNotSupported)
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	// An otherwise fully-actuatable hardware aux (label + type + aux id).
	auto aux = std::make_shared<AqualinkAutomate::Kernel::AuxillaryDevice>();
	aux->AuxillaryTraits.Set(LabelTrait{}, std::string{ "Pool Light" });
	aux->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	aux->AuxillaryTraits.Set(AqualinkAutomate::Auxillaries::JandyAuxillaryId{}, AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_5);

	// Emulated and actively polled (running from construction): the gate passes and the
	// command is accepted -- proving the aux IS mappable, so it is the gate (not a mapping
	// failure) that blocks the not-running case below.
	StoppableSerialAdapterDevice running(device_type, *this, true);
	BOOST_CHECK(running.ActuateDevice(aux, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);

	// Emulated but the master is NOT polling our address (watchdog stopped): the queued
	// command could never be transmitted, so ActuateDevice must report NotSupported, never
	// a false Accepted.
	StoppableSerialAdapterDevice not_polled(device_type, *this, true);
	not_polled.SimulateNotPolled();
	BOOST_CHECK(not_polled.ActuateDevice(aux, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(not_polled.ActuateDevice(aux, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(not_polled.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);
}

// =============================================================================
// QueueSetpointCommand
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueSetpointCommand_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueSetpointCommand(SerialAdapter_SystemTemperatureCommands::POOLSP, 82));
}

BOOST_AUTO_TEST_CASE(TestQueueSetpointCommand_SpaSetpoint)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueSetpointCommand(SerialAdapter_SystemTemperatureCommands::SPASP, 100));
}

// =============================================================================
// Command sequencing
// =============================================================================

BOOST_AUTO_TEST_CASE(TestCommandSequence_PumpThenAux)
{
	SerialAdapterDevice device(device_type, *this, true);
	device.QueuePumpCommand(SerialAdapter_SystemPumpCommands::PUMP, SerialAdapter_CommandTypes::SetOn);
	BOOST_CHECK_NO_THROW(device.QueueAuxCommand(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_1, SerialAdapter_CommandTypes::SetOn));
}

// =============================================================================
// Destruction after queuing
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDestruction_AfterQueuing)
{
	{
		SerialAdapterDevice device(device_type, *this, true);
		device.QueueCommand(0x05, 0x0A);
		device.QueuePumpCommand(SerialAdapter_SystemPumpCommands::PUMP, SerialAdapter_CommandTypes::Toggle);
		device.QueueAuxCommand(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_2, SerialAdapter_CommandTypes::SetOff);
		device.QueueSetpointCommand(SerialAdapter_SystemTemperatureCommands::POOLSP, 82);
	}
	BOOST_CHECK(true);
}

// =============================================================================
// Different device ID
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_DifferentDeviceId)
{
	auto device_type_49 = std::make_shared<JandyDeviceType>(JandyDeviceId(0x49));
	BOOST_CHECK_NO_THROW(SerialAdapterDevice device(device_type_49, *this, true));
}

// =============================================================================
// Presence gating: SuppressEmulation latch on the Emulated capability
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPresenceGating_EmulatedDeviceStartsActiveNotSuppressed)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK(device.IsEmulated());
	BOOST_CHECK(!device.IsEmulationSuppressed());
	BOOST_CHECK(device.IsEmulationActive());
}

BOOST_AUTO_TEST_CASE(TestPresenceGating_SuppressEmulationIsOneWayLatch)
{
	SerialAdapterDevice device(device_type, *this, true);

	device.SuppressEmulation();
	BOOST_CHECK(device.IsEmulated());          // construction intent is unchanged
	BOOST_CHECK(device.IsEmulationSuppressed());
	BOOST_CHECK(!device.IsEmulationActive());

	// Idempotent: calling again keeps it suppressed.
	device.SuppressEmulation();
	BOOST_CHECK(device.IsEmulationSuppressed());
	BOOST_CHECK(!device.IsEmulationActive());
}

BOOST_AUTO_TEST_CASE(TestPresenceGating_NonEmulatedDeviceIsNeverActive)
{
	SerialAdapterDevice device(device_type, *this, false);
	BOOST_CHECK(!device.IsEmulated());
	BOOST_CHECK(!device.IsEmulationActive());
}

// =============================================================================
// Capture-gated write methods (no-throw queuing)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueSetpointWriteTwoStep_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueSetpointWrite_TwoStep(SerialAdapter_SystemTemperatureCommands::POOLSP, 82));
}

BOOST_AUTO_TEST_CASE(TestQueueAuxToggleWrite_DoesNotThrow)
{
	SerialAdapterDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueAuxToggleWrite(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_1, true));
	BOOST_CHECK_NO_THROW(device.QueueAuxToggleWrite(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_2, false));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// REGRESSION: a status QUERY is not evidence that the queried relay exists.
//
// The round-robin poll list is built from every value of JandyAuxillaryIds, so
// the adapter asks about all 32 numbered relays plus ExtraAux. A reply used to
// be turned straight into a device, and because the reply carries no name the
// factory fell back to the enum name -- so a single-power-centre panel (an RS-8
// Combo: 7 relays, 1 centre) grew a full set of phantom "Aux B1" ... "Aux D8"
// entries, all of which reached the web UI, MQTT and Home Assistant discovery
// and persisted in the equipment cache.
//
// Discovery is now bounded by the model the PANEL ITSELF reports.
//=============================================================================

namespace
{
	namespace Aux = AqualinkAutomate::Auxillaries;

	constexpr uint8_t RSSA_DEVICE_ID = 0x48;
	constexpr uint8_t RSSA_STATUS_TYPE_ABOUT_DEVICE = 0x03;

	// An RSSA_DevStatus (0x13) reply about one aux relay. Full-message layout:
	// [4]=status type, [6]=aux state, [7]=aux id + SERIALADAPTER_AUX_ID_OFFSET.
	std::vector<uint8_t> MakeAuxStatusFrame(Aux::JandyAuxillaryIds aux_id, bool is_on)
	{
		const std::vector<uint8_t> payload = {
			RSSA_STATUS_TYPE_ABOUT_DEVICE,
			0x00,
			static_cast<uint8_t>(is_on ? Aux::JandyAuxillaryStatuses::On : Aux::JandyAuxillaryStatuses::Off),
			static_cast<uint8_t>(static_cast<uint8_t>(aux_id) + SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET)
		};

		return AqualinkAutomate::Test::MessageBuilder::CreateValidChecksummedMessage(
			RSSA_DEVICE_ID,
			static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::RSSA_DevStatus),
			payload);
	}

	// Identify the panel exactly as the OneTouch version-page scrape does.
	void IdentifyAsRS8Combo(AqualinkAutomate::Kernel::DataHub& data_hub)
	{
		data_hub.SystemBoard = AqualinkAutomate::Kernel::SystemBoards::RS8_Combo;
		data_hub.ExpectedAuxillaryCount = 7;
		data_hub.ExpectedPowerCenterCount = 1;
	}
}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(SerialAdapterDevice_AuxDiscoverySpan_TestSuite)

BOOST_AUTO_TEST_CASE(AuxStatusForARelayTheModelHas_CreatesTheDevice)
{
	AqualinkAutomate::Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(RSSA_DEVICE_ID));
	SerialAdapterDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);

	IdentifyAsRS8Combo(*harness.DataHub());

	harness.Replay(MakeAuxStatusFrame(Aux::JandyAuxillaryIds::Aux_5, /*is_on=*/true));

	auto created = harness.DataHub()->Devices.FindById(Aux::AuxStableId(Aux::JandyAuxillaryIds::Aux_5));
	BOOST_REQUIRE(nullptr != created);

	auto status = created->AuxillaryTraits.TryGet(AqualinkAutomate::Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == AqualinkAutomate::Kernel::AuxillaryStatuses::On);
}

BOOST_AUTO_TEST_CASE(AuxStatusForARelayTheModelCannotHave_CreatesNothing)
{
	AqualinkAutomate::Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(RSSA_DEVICE_ID));
	SerialAdapterDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);

	IdentifyAsRS8Combo(*harness.DataHub());

	// Banks B/C/D live on power centres an RS-8 Combo does not have.
	for (const auto aux_id : { Aux::JandyAuxillaryIds::Aux_B1, Aux::JandyAuxillaryIds::Aux_B2,
	                           Aux::JandyAuxillaryIds::Aux_C4, Aux::JandyAuxillaryIds::Aux_D6 })
	{
		harness.Replay(MakeAuxStatusFrame(aux_id, /*is_on=*/false));
		BOOST_CHECK(nullptr == harness.DataHub()->Devices.FindById(Aux::AuxStableId(aux_id)));
	}

	BOOST_CHECK(harness.DataHub()->Auxillaries().empty());
}

BOOST_AUTO_TEST_CASE(ExtraAuxIsStillDiscoverable)
{
	// ExtraAux belongs to no numbered power centre, so the model's relay span cannot judge it.
	// It is a real relay on panels that have one (the solar booster, per the AquaLink RS
	// manual), and whether a given panel does is a separate capture-gated question -- so
	// discovery must NOT silently drop it just because a relay-count table does not list it.
	AqualinkAutomate::Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(RSSA_DEVICE_ID));
	SerialAdapterDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);

	IdentifyAsRS8Combo(*harness.DataHub());

	harness.Replay(MakeAuxStatusFrame(Aux::JandyAuxillaryIds::ExtraAux, /*is_on=*/false));

	BOOST_CHECK(nullptr != harness.DataHub()->Devices.FindById(Aux::AuxStableId(Aux::JandyAuxillaryIds::ExtraAux)));
}

BOOST_AUTO_TEST_CASE(BeforeThePanelIsIdentified_EveryRelayIsStillDiscoverable)
{
	// An RSSA-only rig has no other enumerating source, and until the version page has been
	// scraped there is no model to bound the sweep by -- so nothing may be excluded.
	AqualinkAutomate::Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(RSSA_DEVICE_ID));
	SerialAdapterDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);

	harness.Replay(MakeAuxStatusFrame(Aux::JandyAuxillaryIds::Aux_B1, /*is_on=*/true));

	BOOST_CHECK(nullptr != harness.DataHub()->Devices.FindById(Aux::AuxStableId(Aux::JandyAuxillaryIds::Aux_B1)));
}

BOOST_AUTO_TEST_SUITE_END()
