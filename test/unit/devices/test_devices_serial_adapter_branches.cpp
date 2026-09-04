#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/serial_adapter_device.h"
#include "devices/device_status.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/serial_adapter/serial_adapter_message_dev_status.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "kernel/pool_configurations.h"
#include "kernel/system_boards.h"
#include "kernel/temperature.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Messages;

//=============================================================================
// SerialAdapterDevice -- the inbound slot / poll-ACK branches.
//
// The existing suite drives the public queue API and the aux-discovery span; it
// never delivers a master poll (CMD_STATUS 0x02 / Probe 0x00 / DEV_READY 0x07) to
// the adapter, so the round-robin status query, the pending-command drain, the
// DEV_READY two-step handshake, presence gating and the DevStatus field decodes
// were all unexercised. These cases replay real frames through the full protocol
// stack (MockReplayHarness) and observe the ACK the emulated adapter puts on the
// wire via the JandyMessage_Ack send-publisher.
//=============================================================================

namespace
{
	namespace Aux = AqualinkAutomate::Auxillaries;
	namespace Traits = AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	constexpr uint8_t RSSA_ID = 0x48;
	constexpr uint8_t CMD_PROBE = 0x00;
	constexpr uint8_t CMD_ACK = 0x01;
	constexpr uint8_t CMD_STATUS = 0x02;
	constexpr uint8_t CMD_DEV_READY = 0x07;
	constexpr uint8_t CMD_DEV_STATUS = 0x13;
	constexpr uint8_t CMD_UNKNOWN_1B = 0x1B;

	constexpr uint8_t CMD_TYPE_QUERY = 0x05;
	constexpr uint8_t CMD_TYPE_SET_OFF = 0x80;
	constexpr uint8_t CMD_TYPE_SET_ON = 0x81;
	constexpr uint8_t CMD_TYPE_READY_SP = 0x35;

	constexpr uint8_t AUX_ID_OFFSET = SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET;

	using Frame = std::vector<uint8_t>;

	// A master CMD_STATUS (0x02) poll addressed to the adapter (minimum 5-byte status payload).
	Frame StatusPoll() { return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_STATUS, { 0x00, 0x00, 0x00, 0x00, 0x00 }); }
	Frame Probe() { return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_PROBE, {}); }
	Frame DevReady() { return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_DEV_READY, { 0x00, 0x00 }); }
	Frame UnknownFrame() { return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_UNKNOWN_1B, { 0x00 }); }
	Frame AckToAdapter() { return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_ACK, { 0x00, 0x00 }); }

	// An RSSA_DevStatus (0x13) reply. Full-message layout: [4]=status type, [5..7]=fields.
	Frame DevStatus(uint8_t status_type, uint8_t b5, uint8_t b6, uint8_t b7 = 0x00)
	{
		return Test::MessageBuilder::CreateValidChecksummedMessage(RSSA_ID, CMD_DEV_STATUS, { status_type, b5, b6, b7 });
	}

	// A DevStatus carrying a SystemTemperatureCommands value in the shared value slot ([6]).
	Frame TempStatus(SerialAdapter_SystemTemperatureCommands stc, uint8_t value)
	{
		return DevStatus(static_cast<uint8_t>(stc), 0x00, value);
	}

	// Records every ACK the emulated adapter transmits: (raw ack_type byte, data byte).
	//
	// The raw bytes are recovered via SerializeContents (the idiom used by
	// test/integration/devices/test_serial_adapter_device_commands.cpp) rather than via
	// JandyMessage_Ack::AckType(): that accessor runs the byte through
	// magic_enum::enum_cast<AckTypes> and collapses every value not named in AckTypes to
	// AckTypes::Unknown (0xFF). Nearly all of the RSSA status/setpoint codes asserted here
	// (POOLSP 0x05, SPASP 0x07, OPMODE 0x0D, CLEANR 0x10, ...) are absent from AckTypes, so
	// reading them through AckType() would compare 0xFF against everything.
	struct AckRecorder
	{
		AckRecorder()
		{
			connection = JandyMessage_Ack::GetPublisher()->connect(
				[this](std::reference_wrapper<const JandyMessage_Ack> r)
				{
					std::vector<uint8_t> serialised;
					r.get().SerializeContents(serialised);
					if (2u <= serialised.size())
					{
						acks.emplace_back(serialised[0], serialised[1]);
					}
				});
		}

		std::vector<std::pair<uint8_t, uint8_t>> acks;
		boost::signals2::scoped_connection connection;
	};

	// Identify the panel exactly as the OneTouch version-page scrape does (RS-8 Combo: 7 aux, 1 centre).
	void IdentifyAsRS8Combo(Kernel::DataHub& data_hub)
	{
		data_hub.SystemBoard = Kernel::SystemBoards::RS8_Combo;
		data_hub.ExpectedAuxillaryCount = 7;
		data_hub.ExpectedPowerCenterCount = 1;
	}

	std::shared_ptr<Kernel::AuxillaryDevice> MakeTypedDevice(Traits::AuxillaryTypes type, const std::string& label)
	{
		auto device = std::make_shared<Kernel::AuxillaryDevice>();
		device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		device->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ label });
		return device;
	}

	struct AdapterFixture
	{
		AdapterFixture() :
			id(std::make_shared<JandyDeviceType>(JandyDeviceId(RSSA_ID)))
		{
		}

		Test::MockReplayHarness harness;
		std::shared_ptr<JandyDeviceType> id;
		AckRecorder recorder;
	};
}
// unnamed namespace

BOOST_FIXTURE_TEST_SUITE(SerialAdapterDevice_Branches_TestSuite, AdapterFixture)

//-----------------------------------------------------------------------------
// CMD_STATUS poll: pending command drain, then the round-robin status query
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(StatusPoll_DrainsQueuedCommand_ThenQueriesNextStatusType)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	device.QueueSetpointCommand(SerialAdapter_SystemTemperatureCommands::POOLSP, 82);
	BOOST_CHECK(device.DescribeDiagnostics()["has_pending_command"].get<bool>());

	// The first poll after the queue carries the pending command.
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(SerialAdapter_SystemTemperatureCommands::POOLSP));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), 82);
	BOOST_CHECK(!device.DescribeDiagnostics()["has_pending_command"].get<bool>());

	// With nothing pending the next poll asks for the first status type in the rotation:
	// OPTIONS (MODEL is dropped from the rotation at construction) with the Query code.
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), static_cast<int>(SerialAdapter_SystemConfigurationStatuses::OPTIONS));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].second), static_cast<int>(CMD_TYPE_QUERY));

	// ...and then advances to the next entry (OPMODE).
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 3u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[2].first), static_cast<int>(SerialAdapter_SystemConfigurationStatuses::OPMODE));
}

BOOST_AUTO_TEST_CASE(StatusPolls_WrapTheRotation_AndMarkTheDeviceNormal)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	BOOST_REQUIRE(DeviceStatus_Initializing{} == device.Status());

	// One full sweep of the status rotation (config + temperature + every aux id) is
	// well under 60 polls; wrapping the cursor promotes the device to Normal.
	for (int i = 0; i < 60; ++i)
	{
		harness.Replay(StatusPoll());
	}

	BOOST_CHECK(DeviceStatus_Normal{} == device.Status());
	BOOST_CHECK_EQUAL(recorder.acks.size(), 60u);

	// Every ack in the sweep was a status query (Query code, or an aux query 0x00/devID).
	for (const auto& [type, data] : recorder.acks)
	{
		const bool is_query = (data == CMD_TYPE_QUERY) || ((type == 0x00) && (data >= AUX_ID_OFFSET));
		BOOST_CHECK(is_query);
	}
}

BOOST_AUTO_TEST_CASE(StatusPolls_OnIdentifiedPanel_SkipAuxRelaysTheModelCannotHave)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	IdentifyAsRS8Combo(*harness.DataHub());

	for (int i = 0; i < 60; ++i)
	{
		harness.Replay(StatusPoll());
	}

	// Aux queries are {ack_type=0x00, data=aux+offset}; on an RS-8 Combo (7 relays, one power
	// centre) banks B/C/D (aux ids 8..31) must never be asked about, but Aux1..7 must be.
	bool saw_bank_a = false;
	for (const auto& [type, data] : recorder.acks)
	{
		if ((type == 0x00) && (data >= AUX_ID_OFFSET))
		{
			const int aux_id = static_cast<int>(data) - static_cast<int>(AUX_ID_OFFSET);
			BOOST_CHECK(aux_id < 8);
			saw_bank_a = saw_bank_a || ((aux_id >= 1) && (aux_id <= 7));
		}
	}
	BOOST_CHECK(saw_bank_a);
	BOOST_CHECK(DeviceStatus_Normal{} == device.Status());
}

//-----------------------------------------------------------------------------
// Probe / Unknown / Ack slots
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Probe_AnswersWithIdleAck_WithoutDrainingTheQueue)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	device.QueueCommand(0x05, 0x0A);

	// A probe is not a CMD_STATUS: the adapter answers, but the pending command waits for a
	// status poll (m_StatusMessageReceived gates the drain).
	harness.Replay(Probe());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), 0x00);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), 0x00);
	BOOST_CHECK(device.DescribeDiagnostics()["has_pending_command"].get<bool>());
}

BOOST_AUTO_TEST_CASE(UnknownFrame_AddressedToAdapter_AnswersWithIdleAck)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	harness.Replay(UnknownFrame());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), 0x00);
	BOOST_CHECK(device.DescribeDiagnostics()["is_running"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Ack_ObservedForRealAdapter_KeepsWatchdogAlive)
{
	// A passive (non-emulated) adapter registers the Ack slot: seeing the real adapter's ACK
	// at its address only kicks the watchdog; nothing is transmitted.
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);

	harness.Replay(AckToAdapter());
	BOOST_CHECK(recorder.acks.empty());
	BOOST_CHECK(device.DescribeDiagnostics()["is_running"].get<bool>());
}

//-----------------------------------------------------------------------------
// DEV_READY: the second half of the two-step setpoint handshake
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DevReady_AfterAddressClaimed_DrainsTheSetSpStep)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_REQUIRE(device.SetSpaSetpoint(100) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 2u);

	// CMD_STATUS -> readySP {SPASP, 0x35}; this also claims the bus address.
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(SerialAdapter_SystemTemperatureCommands::SPASP));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(CMD_TYPE_READY_SP));

	// DEV_READY is now SOLICITED (we own the address) -> not a collision; the setSP value
	// {0x00, 100} goes out in reply to THIS poll.
	harness.Replay(DevReady());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), 0x00);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].second), 100);
	BOOST_CHECK(!device.IsEmulationSuppressed());
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 0u);
}

BOOST_AUTO_TEST_CASE(DevReady_Unsolicited_SuppressesEmulationAndDropsQueuedWork)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	device.QueueCommand(0x05, 0x0A);

	// A DEV_READY at our address BEFORE we ever answered a poll means a real adapter is
	// already conversing here: go passive, drop the queue, transmit nothing.
	harness.Replay(DevReady());
	BOOST_CHECK(device.IsEmulationSuppressed());
	BOOST_CHECK(!device.IsEmulationActive());
	BOOST_CHECK(recorder.acks.empty());
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 0u);

	// Once suppressed, new commands are refused at the queue.
	device.QueueCommand(0x05, 0x0A);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 0u);
	device.QueueSetpointWrite_TwoStep(SerialAdapter_SystemTemperatureCommands::POOLSP, 82);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 0u);

	// ...and the capability surface reports the honest fallback result.
	BOOST_CHECK(device.SetSpaSetpoint(100) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetCirculationMode(Kernel::CirculationModes::Pool) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetHeaterMode(Kernel::BodyOfWaterIds::Pool, true) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(DevStatus_WithPresenceGatingDisabled_NeverSuppresses)
{
	harness.DataHub()->PresenceGatingDisabled = true;
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	// OPMODE = Service, unsolicited -- with the opt-out set it is decoded, not treated as a collision.
	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::OPMODE), 0x00, static_cast<uint8_t>(SerialAdapter_SCS_OpModes::Service)));

	BOOST_CHECK(!device.IsEmulationSuppressed());
	BOOST_CHECK(harness.DataHub()->Mode == Kernel::EquipmentMode::Service);
	// The DevStatus processing ends with a controller update, which transmits when active.
	BOOST_CHECK_EQUAL(recorder.acks.size(), 1u);
}

//-----------------------------------------------------------------------------
// DevStatus field decodes -> DataHub
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DevStatus_Options_RewritesTheCleanerQuery_AndDropsOptions)
{
	harness.DataHub()->PresenceGatingDisabled = true;
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	const auto before = device.DescribeDiagnostics()["status_collection_count"].get<uint32_t>();

	// OPTIONS reporting a cleaner (HasCleaner, bit 0). AUX1 is then wired as the CLEANER, so
	// asking about AUX1 by relay id errors -- the rotation entry must be rewritten to the
	// CLEANR pump query.
	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::OPTIONS), 0x00, 0x01));
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["status_collection_count"].get<uint32_t>(), before - 1);

	// A second OPTIONS reply must NOT erase whatever now sits at the front of the rotation.
	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::OPTIONS), 0x00, 0x01));
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["status_collection_count"].get<uint32_t>(), before - 1);

	// Sweep the whole rotation: AUX1 is polled as CLEANR instead of as the raw aux id.
	for (int i = 0; i < 60; ++i)
	{
		harness.Replay(StatusPoll());
	}

	bool saw_cleaner = false;
	for (const auto& [type, data] : recorder.acks)
	{
		if ((type == static_cast<uint8_t>(SerialAdapter_SystemPumpCommands::CLEANR)) && (data == CMD_TYPE_QUERY)) { saw_cleaner = true; }
		if (type == 0x00)
		{
			BOOST_CHECK(data != static_cast<uint8_t>(AUX_ID_OFFSET + 1));   // Aux1 never asked raw
		}
	}
	BOOST_CHECK(saw_cleaner);

	// NOTE: the sibling AUX3 -> SPILLOVER rewrite (Slot_SerialAdapter_DevStatus, guarded by
	// msg.Options().value().HasSpillover) is NOT asserted here because it is currently
	// unreachable for ANY options byte. SerialAdapterMessage_DevStatus::DeserializeContents
	// builds the options struct with
	//     m_Options = static_cast<SerialAdapter_SCS_Options>(message_bytes[Index_Options]);
	// which is a C++20 parenthesised aggregate initialisation of a struct of eight bool
	// bit-fields: the WHOLE byte is converted to bool and stored in the first member
	// (HasCleaner), and the remaining seven -- HasSpillover included -- are value-initialised
	// to false. The byte is a bitmask (the adjacent LogDebug prints it as {:08B}), so this is a
	// production decode defect, not a test-expectation problem; asserting the current behaviour
	// would bake the defect in. Once the decode masks bit-by-bit, add the SPILLOVER rewrite
	// assertion (and an AUX3-never-asked-raw check) alongside the cleaner ones above.
}

BOOST_AUTO_TEST_CASE(DevStatus_CelsiusTemperaturesAndSetpoints_PopulateDataHub)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	auto data_hub = harness.DataHub();

	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::UNITS, 0x01));   // Celsius
	BOOST_REQUIRE(data_hub->SystemTemperatureUnits() == Kernel::TemperatureUnits::Celsius);

	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLSP, 28));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLSP2, 26));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLHT2, 0x01));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::SPASP, 38));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::AIRTMP, 24));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLTMP, 27));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::SPATMP, 36));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::SOLTMP, 30));   // decoded, not surfaced

	BOOST_REQUIRE(data_hub->PoolTempSetpoint().has_value());
	BOOST_CHECK_CLOSE(data_hub->PoolTempSetpoint()->InCelsius().value(), 28.0, 0.01);
	BOOST_REQUIRE(data_hub->PoolTempSetpoint2().has_value());
	BOOST_CHECK_CLOSE(data_hub->PoolTempSetpoint2()->InCelsius().value(), 26.0, 0.01);
	BOOST_REQUIRE(data_hub->PoolHeater2Enabled().has_value());
	BOOST_CHECK(data_hub->PoolHeater2Enabled().value());
	BOOST_REQUIRE(data_hub->SpaTempSetpoint().has_value());
	BOOST_CHECK_CLOSE(data_hub->SpaTempSetpoint()->InCelsius().value(), 38.0, 0.01);
	BOOST_REQUIRE(data_hub->AirTemp().has_value());
	BOOST_CHECK_CLOSE(data_hub->AirTemp()->InCelsius().value(), 24.0, 0.01);
	BOOST_REQUIRE(data_hub->PoolTemp().has_value());
	BOOST_CHECK_CLOSE(data_hub->PoolTemp()->InCelsius().value(), 27.0, 0.01);
	BOOST_REQUIRE(data_hub->SpaTemp().has_value());
	BOOST_CHECK_CLOSE(data_hub->SpaTemp()->InCelsius().value(), 36.0, 0.01);

	// A raw 0 means "sensor not available" and must not overwrite the last real reading.
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::AIRTMP, 0));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLTMP, 0));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::SPATMP, 0));
	BOOST_CHECK_CLOSE(data_hub->AirTemp()->InCelsius().value(), 24.0, 0.01);
	BOOST_CHECK_CLOSE(data_hub->PoolTemp()->InCelsius().value(), 27.0, 0.01);
	BOOST_CHECK_CLOSE(data_hub->SpaTemp()->InCelsius().value(), 36.0, 0.01);

	// POOLHT2 = 0 -> maintenance heating disabled.
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLHT2, 0x00));
	BOOST_CHECK(!data_hub->PoolHeater2Enabled().value());
}

BOOST_AUTO_TEST_CASE(DevStatus_FahrenheitUnits_ConvertRawDegreesAsFahrenheit)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	auto data_hub = harness.DataHub();

	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::UNITS, 0x00));   // Fahrenheit
	BOOST_REQUIRE(data_hub->SystemTemperatureUnits() == Kernel::TemperatureUnits::Fahrenheit);

	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLTMP, 80));
	harness.Replay(TempStatus(SerialAdapter_SystemTemperatureCommands::POOLSP, 84));

	BOOST_REQUIRE(data_hub->PoolTemp().has_value());
	BOOST_CHECK_CLOSE(data_hub->PoolTemp()->InFahrenheit().value(), 80.0, 0.01);
	BOOST_REQUIRE(data_hub->PoolTempSetpoint().has_value());
	BOOST_CHECK_CLOSE(data_hub->PoolTempSetpoint()->InFahrenheit().value(), 84.0, 0.01);
}

BOOST_AUTO_TEST_CASE(DevStatus_ModelOpModeAndBattery_AreDecoded)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	auto data_hub = harness.DataHub();

	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::MODEL), 0x12, 0x34));
	BOOST_CHECK_EQUAL(data_hub->EquipmentVersions.Get("Model"), std::string("0x1234"));

	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::OPMODE), 0x00, static_cast<uint8_t>(SerialAdapter_SCS_OpModes::Timeout)));
	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::TimeOut);

	harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::OPMODE), 0x00, static_cast<uint8_t>(SerialAdapter_SCS_OpModes::Auto)));
	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::Normal);

	// VBAT is decoded by the message (low flag + voltage) and consumed as a no-op; must not throw.
	BOOST_CHECK_NO_THROW(harness.Replay(DevStatus(static_cast<uint8_t>(SerialAdapter_SystemConfigurationStatuses::VBAT), 0x05, 0x2C)));
	BOOST_CHECK(device.DescribeDiagnostics()["is_running"].get<bool>());
}

//-----------------------------------------------------------------------------
// ActuateDevice: pump / cleaner / spillover / spa mappings and toggle resolution
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ActuateDevice_FilterPumpByTypeAndLabel_QueuesPumpCommand)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	auto pump = MakeTypedDevice(Traits::AuxillaryTypes::Pump, "Filter Pump");

	BOOST_REQUIRE(device.ActuateDevice(pump, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(CMD_TYPE_SET_ON));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(SerialAdapter_SystemPumpCommands::PUMP));

	// An explicit Off maps to the SetOff state byte.
	BOOST_REQUIRE(device.ActuateDevice(pump, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), static_cast<int>(CMD_TYPE_SET_OFF));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].second), static_cast<int>(SerialAdapter_SystemPumpCommands::PUMP));
}

BOOST_AUTO_TEST_CASE(ActuateDevice_PumpToggle_UsesRunningStatusToPickSetOff)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	auto pump = MakeTypedDevice(Traits::AuxillaryTypes::Pump, "Booster Pump");
	pump->AuxillaryTraits.Set(Traits::PumpStatusTrait{}, Kernel::PumpStatuses::Running);

	BOOST_REQUIRE(device.ActuateDevice(pump, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(CMD_TYPE_SET_OFF));

	// Not running -> toggle means SetOn.
	pump->AuxillaryTraits.Set(Traits::PumpStatusTrait{}, Kernel::PumpStatuses::Off);
	BOOST_REQUIRE(device.ActuateDevice(pump, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), static_cast<int>(CMD_TYPE_SET_ON));
}

BOOST_AUTO_TEST_CASE(ActuateDevice_CleanerAndSpillover_MapToTheirPumpCommands)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto cleaner = MakeTypedDevice(Traits::AuxillaryTypes::Cleaner, "Cleaner");
	BOOST_REQUIRE(device.ActuateDevice(cleaner, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(SerialAdapter_SystemPumpCommands::CLEANR));

	auto spillover = MakeTypedDevice(Traits::AuxillaryTypes::Spillover, "Spillover");
	spillover->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);
	// Toggle on an already-ON spillover resolves to SetOff.
	BOOST_REQUIRE(device.ActuateDevice(spillover, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), static_cast<int>(CMD_TYPE_SET_OFF));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].second), static_cast<int>(SerialAdapter_SystemPumpCommands::SPILLOVER));
}

BOOST_AUTO_TEST_CASE(ActuateDevice_SpaByLabel_MapsToSpaCommand_OtherwiseMappingFailed)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto spa = MakeTypedDevice(Traits::AuxillaryTypes::Sprinkler, "Spa Jets");
	BOOST_REQUIRE(device.ActuateDevice(spa, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(SerialAdapter_SystemPumpCommands::SPA));

	// A pump whose label is neither Filter/Pump nor Spa cannot be mapped.
	auto booster = MakeTypedDevice(Traits::AuxillaryTypes::Pump, "Booster");
	BOOST_CHECK(device.ActuateDevice(booster, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::MappingFailed);

	// A typed device with no label at all falls through the same way.
	auto unlabelled = std::make_shared<Kernel::AuxillaryDevice>();
	unlabelled->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Light);
	BOOST_CHECK(device.ActuateDevice(unlabelled, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::MappingFailed);
}

BOOST_AUTO_TEST_CASE(ActuateDevice_HardwareAuxToggle_WhenOn_EmitsSetDevOff)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto aux = MakeTypedDevice(Traits::AuxillaryTypes::Auxillary, "Pool Light");
	aux->AuxillaryTraits.Set(Aux::JandyAuxillaryId{}, Aux::JandyAuxillaryIds::Aux_5);
	aux->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);

	BOOST_REQUIRE(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(CMD_TYPE_SET_OFF));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(AUX_ID_OFFSET + 5));

	// Explicit Off on a hardware aux also emits the setDev OFF frame.
	BOOST_REQUIRE(device.ActuateDevice(aux, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), static_cast<int>(CMD_TYPE_SET_OFF));
}

//-----------------------------------------------------------------------------
// Heater / circulation / setpoint capability edges
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(SetHeaterMode_Disable_EmitsSetDevOffForHeaterCode)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_REQUIRE(device.SetHeaterMode(Kernel::BodyOfWaterIds::Spa, false) == Capabilities::ActuationResult::Accepted);
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(CMD_TYPE_SET_OFF));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(SerialAdapter_SystemTemperatureCommands::SPAHT));
}

BOOST_AUTO_TEST_CASE(SetCirculationMode_UnknownEnumerator_IsInvalidValue)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	const auto bogus = static_cast<Kernel::CirculationModes>(250);
	BOOST_CHECK(device.SetCirculationMode(bogus) == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 0u);
}

BOOST_AUTO_TEST_CASE(SetSpaSetpoint_Emulated_QueuesReadySpThenSetSp)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_REQUIRE(device.SetSpaSetpoint(38) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["pending_command_count"].get<uint32_t>(), 2u);

	// Both steps drain in order across two CMD_STATUS polls.
	harness.Replay(StatusPoll());
	harness.Replay(StatusPoll());
	BOOST_REQUIRE_EQUAL(recorder.acks.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].first), static_cast<int>(SerialAdapter_SystemTemperatureCommands::SPASP));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[0].second), static_cast<int>(CMD_TYPE_READY_SP));
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].first), 0x00);
	BOOST_CHECK_EQUAL(static_cast<int>(recorder.acks[1].second), 38);
}

BOOST_AUTO_TEST_CASE(SetSpaSetpoint_Passive_IsNotSupported)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	BOOST_CHECK(device.SetSpaSetpoint(38) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(device.SetPoolSetpoint(30) == Capabilities::ActuationResult::NotSupported);
}

//-----------------------------------------------------------------------------
// Diagnostics
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DescribeDiagnostics_ReportsQueueAndEmulationState)
{
	SerialAdapterDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto j = device.DescribeDiagnostics();
	BOOST_CHECK_EQUAL(j["device_type"].get<std::string>(), std::string("SerialAdapter"));
	BOOST_CHECK_EQUAL(j["device_id"].get<std::string>(), std::string("0x48"));
	BOOST_CHECK_GT(j["status_collection_count"].get<uint32_t>(), 0u);
	BOOST_CHECK(!j["status_message_received"].get<bool>());
	BOOST_CHECK(!j["has_pending_command"].get<bool>());
	BOOST_CHECK_EQUAL(j["pending_command_count"].get<uint32_t>(), 0u);
	BOOST_CHECK(j["is_emulated"].get<bool>());
	BOOST_CHECK(!j["emulation_suppressed"].get<bool>());
	BOOST_CHECK(j["is_running"].get<bool>());
	BOOST_CHECK(j["recent_commands"].is_array());

	device.QueueAuxToggleWrite(Aux::JandyAuxillaryIds::Aux_2, true);
	device.SuppressEmulation();

	j = device.DescribeDiagnostics();
	BOOST_CHECK(j["has_pending_command"].get<bool>());
	BOOST_CHECK_EQUAL(j["pending_command_count"].get<uint32_t>(), 1u);
	BOOST_CHECK(j["emulation_suppressed"].get<bool>());
}

BOOST_AUTO_TEST_SUITE_END()
