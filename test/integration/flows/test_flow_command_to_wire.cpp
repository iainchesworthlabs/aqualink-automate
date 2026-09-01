#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/test/unit_test.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/statistics_hub.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "protocol/message_generator_registry.h"
#include "protocol/protocol_thread.h"
#include "serial/serial_port.h"

#include "jandy/protocol/jandy_protocol_registration.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/devices/command_dispatcher.h"
#include "jandy/devices/iaq_device.h"
#include "jandy/devices/serial_adapter_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/iaq/iaq_message_control_data_response.h"

#include "mocks/mock_testserialportimpl.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Interfaces;
using namespace AqualinkAutomate::Kernel;

//=============================================================================
// Command-plumbing END-TO-END tests: ICommandDispatcher -> emulated device ->
// Protocol::ProtocolTask -> Serial::SerialPort -> THE WIRE.
//
// Unlike test/integration/devices/test_*_device_commands.cpp (which capture the
// outgoing JandyMessage_Ack at the *signal* level), these tests assert on the
// raw bytes actually WRITTEN to the serial port, via Test::TestSerialPortImpl's
// write capture (GetWrittenData / GetWriteCallCount).  That proves the full
// chain a real command travels: a UI/MQTT caller drives ICommandDispatcher, the
// dispatcher routes to the emulated SerialAdapter/IAQ device, the device emits a
// message in response to a bus poll/status, ProtocolTask serializes+enqueues it,
// and DrainWrites() pushes the framed+checksummed bytes onto the port.
//
// Wiring mirrors production (src/aqualink-automate.cpp):
//   * The emulated devices live in the EquipmentHub (so the CommandDispatcher's
//     FindSerialAdapter()/FindIAQDevice() resolve them, exactly as at runtime).
//   * ProtocolTask::ConnectWriteSignal<JandyMessage_Ack>() and
//     <IAQMessage_ControlDataResponse>() are connected, so emitted messages are
//     serialized and written to the port — nothing is stubbed.
//
// Device ids: SerialAdapter 0x48 (DeviceClasses::SerialAdapter); IAQ 0x33
// (DeviceClasses::AqualinkTouch — the class FindIAQDevice() matches).
//=============================================================================

namespace
{
	constexpr uint8_t SERIAL_ADAPTER_ID = 0x48;
	constexpr uint8_t IAQ_ID = 0x33;

	// AqualinkTouch page ids (cross-validated against captures and AqualinkD's iaqtouch.h).
	constexpr uint8_t IAQ_PAGE_HOME = 0x01;
	constexpr uint8_t IAQ_PAGE_MENU = 0x0f;
	constexpr uint8_t IAQ_PAGE_SET_SWG = 0x30;

	constexpr uint8_t DLE = 0x10;
	constexpr uint8_t STX = 0x02;
	constexpr uint8_t ETX = 0x03;

	// Build a fully-framed+checksummed Jandy packet (same layout MessageBuilder
	// uses) so it passes the real generator's validation when replayed.
	std::vector<uint8_t> MakeFramedPacket(uint8_t dest, uint8_t msg_type, const std::vector<uint8_t>& payload = {})
	{
		std::vector<uint8_t> pkt = { DLE, STX, dest, msg_type };
		pkt.insert(pkt.end(), payload.begin(), payload.end());
		uint32_t sum = 0;
		for (auto b : pkt) { sum += b; }
		pkt.push_back(static_cast<uint8_t>(sum & 0xFF));
		pkt.push_back(DLE);
		pkt.push_back(ETX);
		return pkt;
	}

	// The exact bytes ProtocolTask will write for a given ACK, computed by the
	// SAME production serializer the write path uses.  Comparing the captured
	// wire bytes against this proves the command reached the port intact.
	std::vector<uint8_t> ExpectedAckWireBytes(uint8_t ack_type, uint8_t command)
	{
		Messages::JandyMessage_Ack ack(ack_type, command);
		std::vector<uint8_t> bytes;
		ack.Serialize(bytes);
		return bytes;
	}

	std::vector<uint8_t> ExpectedControlDataWireBytes(const std::string& ascii)
	{
		Messages::IAQMessage_ControlDataResponse msg(ascii);
		std::vector<uint8_t> bytes;
		msg.Serialize(bytes);
		return bytes;
	}

	//-------------------------------------------------------------------------
	// Full-stack fixture: real hubs + serial port + protocol task + dispatcher,
	// with the emulated devices registered in the EquipmentHub.
	//-------------------------------------------------------------------------
	struct CommandToWireFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		CommandToWireFixture()
			: data_hub(Find<DataHub>())
			, equipment_hub(Find<EquipmentHub>())
			, statistics_hub(Find<StatisticsHub>())
			, serial_impl(new Test::TestSerialPortImpl())
			, serial_port(std::make_shared<Serial::SerialPort>(
				std::unique_ptr<Test::TestSerialPortImpl>(serial_impl), *this))
			, dispatcher(std::make_shared<CommandDispatcher>(data_hub, equipment_hub))
		{
			serial_impl->EnableTestMode(true);

			// Register the dispatcher in the HubLocator exactly as production does,
			// so the web routes resolve it via TryFind<ICommandDispatcher>().
			Register<Interfaces::ICommandDispatcher>(dispatcher);

			// Register ONLY the Jandy generator (isolated from any leftover).
			Protocol::MessageGeneratorRegistry::Instance().Clear();
			Jandy::Protocol::RegisterMessageGenerator();

			protocol_task = std::make_shared<Protocol::ProtocolTask>(serial_port, statistics_hub);

			// Connect the write signals exactly as aqualink-automate.cpp does, so
			// emitted ACK / control-data messages are serialized and written.
			protocol_task->ConnectWriteSignal<Messages::JandyMessage_Ack>();
			protocol_task->ConnectWriteSignal<Messages::IAQMessage_ControlDataResponse>();

			// Register the emulated devices in the EquipmentHub (is_emulated=true so
			// they actively ACK).  The CommandDispatcher resolves them from here.
			auto sa_id = std::make_shared<JandyDeviceType>(JandyDeviceId(SERIAL_ADAPTER_ID));
			equipment_hub->AddDevice(std::make_unique<SerialAdapterDevice>(sa_id, *this, true));

			auto iaq_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_ID));
			equipment_hub->AddDevice(std::make_unique<IAQDevice>(iaq_id, *this, true));
		}

		~CommandToWireFixture() override
		{
			protocol_task.reset();
			Protocol::MessageGeneratorRegistry::Instance().Clear();
			if (serial_impl != nullptr)
			{
				serial_impl->Reset();
			}
		}

		// Replay one framed packet through the full protocol task (read -> parse ->
		// fire signal -> device reacts -> write enqueued -> drained to the port).
		void Replay(const std::vector<uint8_t>& frame)
		{
			serial_impl->QueueReadData(frame);
			serial_impl->QueueReadData({}, boost::asio::error::would_block);
			(void)protocol_task->Poll();
		}

		void ReplaySerialAdapterStatus()
		{
			Replay(MakeFramedPacket(SERIAL_ADAPTER_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Status), { 0x00, 0x00, 0x00, 0x00, 0x00 }));
		}

		// The controller solicits the second step of a two-step setpoint write with an
		// RSSA_DEV_READY (0x07) poll carrying the readied setpoint type.
		void ReplaySerialAdapterDevReady(uint8_t readied_type)
		{
			Replay(MakeFramedPacket(SERIAL_ADAPTER_ID, static_cast<uint8_t>(Messages::JandyMessageIds::RSSA_DevReady), { readied_type }));
		}

		void ReplayIAQPoll()
		{
			Replay(MakeFramedPacket(IAQ_ID, static_cast<uint8_t>(Messages::JandyMessageIds::IAQ_Poll)));
		}

		void ReplayIAQControlReady()
		{
			Replay(MakeFramedPacket(IAQ_ID, static_cast<uint8_t>(Messages::JandyMessageIds::IAQ_ControlReady)));
		}

		// Render a page at the IAQ: PageStart (which latches the page id) followed by one
		// PageButton per entry. This is how the master drives the panel UI, and it is what the
		// chlorinator writer gates its navigation on.
		// PageButton payload: [index, state, pad, type, name...].
		void ReplayIAQPageWithButtons(uint8_t page_id, const std::vector<std::pair<uint8_t, std::string>>& buttons)
		{
			Replay(MakeFramedPacket(IAQ_ID, static_cast<uint8_t>(Messages::JandyMessageIds::IAQ_PageStart), { page_id }));

			for (const auto& [index, name] : buttons)
			{
				std::vector<uint8_t> payload = { index, 0x00, 0x00, 0x01 };
				payload.insert(payload.end(), name.begin(), name.end());
				Replay(MakeFramedPacket(IAQ_ID, static_cast<uint8_t>(Messages::JandyMessageIds::IAQ_PageButton), payload));
			}
		}

		// The writer emits ONE command per poll and dwells in between while the master renders,
		// so a test has to poll until something other than an idle ACK reaches the wire.
		void DrainIAQPollsUntilCommand(int max_polls = 10)
		{
			const auto idle = ExpectedAckWireBytes(0x00, 0x00);

			for (int i = 0; i < max_polls; ++i)
			{
				ClearWire();
				ReplayIAQPoll();
				if (Wire() != idle)
				{
					return;
				}
			}
		}

		const std::vector<uint8_t>& Wire() const { return serial_impl->GetWrittenData(); }
		void ClearWire() { serial_impl->ClearWrittenData(); }

		std::shared_ptr<DataHub> data_hub;
		std::shared_ptr<EquipmentHub> equipment_hub;
		std::shared_ptr<StatisticsHub> statistics_hub;

		Test::TestSerialPortImpl* serial_impl;	// owned by serial_port
		std::shared_ptr<Serial::SerialPort> serial_port;
		std::shared_ptr<Protocol::ProtocolTask> protocol_task;
		std::shared_ptr<CommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_Integration_Flow_CommandToWire, CommandToWireFixture)

//-----------------------------------------------------------------------------
// Sanity: with NO command queued, a Status poll still produces a status-query
// ACK on the wire (proves the write path is live end-to-end before we assert
// specific command bytes below).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SerialAdapter_NoCommand_StillWritesQueryAck)
{
	ClearWire();
	ReplaySerialAdapterStatus();

	BOOST_REQUIRE(!Wire().empty());
	// A framed Jandy ACK packet is at least 8 bytes (DLE STX dest type t c chk DLE ETX).
	BOOST_CHECK_GE(Wire().size(), 8u);
	BOOST_CHECK_EQUAL(Wire().front(), DLE);
	BOOST_CHECK_EQUAL(Wire().back(), ETX);
}

//=============================================================================
// SerialAdapter-routed commands: SetPoolSetpoint / SetSpaSetpoint /
// SetCirculationMode all queue a PendingCommand that the device emits as a
// single ACK on the next Status poll.
//=============================================================================

BOOST_AUTO_TEST_CASE(SetPoolSetpoint_WritesTwoStepToWire)
{
	auto result = dispatcher->SetPoolSetpoint(82);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	// Two-step readySP/setSP (AqualinkD queue_aqualink_rssadapter_setpoint), one frame per poll.
	// Step 1 readySP: {POOLSP 0x05, 0x35}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x05, 0x35));

	// Step 2 setSP: {0x00, temperature (82)}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 82));
}

BOOST_AUTO_TEST_CASE(SetSpaSetpoint_WritesTwoStepToWire)
{
	auto result = dispatcher->SetSpaSetpoint(100);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	// Step 1 readySP: {SPASP 0x07, 0x35}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x07, 0x35));

	// Step 2 setSP: {0x00, temperature (100)}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 100));
}

//-----------------------------------------------------------------------------
// REGRESSION (live-validated 2026-06-28, dual-body iAQ/OneTouch + AquaRite):
// the controller answers the readySP step with an RSSA_DEV_READY (0x07) poll
// carrying the readied setpoint type, and EXPECTS the setSP value in reply to
// THAT poll -- not the next CMD_STATUS. Originally the setSP only drained on a
// CMD_STATUS poll, so it missed the DEV_READY window and the controller ignored
// the write (setpoint never changed on the wire/panel despite well-formed bytes).
// These tests pin the fix: the setSP value MUST be emitted in reply to the
// DEV_READY poll. See SerialAdapterDevice::DrainPendingCommandForDevReady.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SetPoolSetpoint_EmitsSetSPOnDevReadyPoll)
{
	auto result = dispatcher->SetPoolSetpoint(82);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	// Step 1 readySP on a CMD_STATUS poll: {POOLSP 0x05, 0x35}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x05, 0x35));

	// Step 2 setSP must be emitted in reply to the DEV_READY (0x07) poll carrying
	// the readied type (0x05), NOT a subsequent CMD_STATUS poll: {0x00, 82}.
	ClearWire();
	ReplaySerialAdapterDevReady(0x05);
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 82));

	// The queue is now drained: a following CMD_STATUS poll returns to status-query
	// rotation rather than re-emitting the setpoint value.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() != ExpectedAckWireBytes(0x00, 82));
}

BOOST_AUTO_TEST_CASE(SetSpaSetpoint_EmitsSetSPOnDevReadyPoll)
{
	auto result = dispatcher->SetSpaSetpoint(100);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	// Step 1 readySP on a CMD_STATUS poll: {SPASP 0x07, 0x35}.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x07, 0x35));

	// Step 2 setSP in reply to the DEV_READY poll carrying the readied type (0x07): {0x00, 100}.
	ClearWire();
	ReplaySerialAdapterDevReady(0x07);
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 100));
}

BOOST_AUTO_TEST_CASE(SetCirculationMode_Spa_WritesSpaOnAckToWire)
{
	auto result = dispatcher->SetCirculationMode(CirculationModes::Spa);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// RSSA setDev body {state, devID}: SetOn = 0x81, SPA device = 0x0E.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x0E));
}

BOOST_AUTO_TEST_CASE(SetCirculationMode_Pool_WritesSpaOffAckToWire)
{
	auto result = dispatcher->SetCirculationMode(CirculationModes::Pool);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// Pool circulation = SPA pump OFF: {state, devID} = SetOff 0x80, SPA 0x0E.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x80, 0x0E));
}

BOOST_AUTO_TEST_CASE(SetCirculationMode_Spillover_WritesSpilloverOnAckToWire)
{
	auto result = dispatcher->SetCirculationMode(CirculationModes::Spillover);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// SPILLOVER: {state, devID} = SetOn 0x81, SPILLOVER 0x0F.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x0F));
}

//=============================================================================
// SetHeaterMode: Pool/Spa/Solar heater enable/disable. RSSA setDev body
// {state, devID} (AqualinkD serialadapter.c rssadapter_device_state): SetOn 0x81 /
// SetOff 0x80 first, heater device code second.
//=============================================================================

BOOST_AUTO_TEST_CASE(SetHeaterMode_PoolOn_WritesPoolHeatOnAckToWire)
{
	auto result = dispatcher->SetHeaterMode(BodyOfWaterIds::Pool, true);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// {state, devID} = SetOn 0x81, POOLHT 0x11.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x11));
}

BOOST_AUTO_TEST_CASE(SetHeaterMode_PoolOff_WritesPoolHeatOffAckToWire)
{
	auto result = dispatcher->SetHeaterMode(BodyOfWaterIds::Pool, false);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// {state, devID} = SetOff 0x80, POOLHT 0x11.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x80, 0x11));
}

BOOST_AUTO_TEST_CASE(SetHeaterMode_SpaOn_WritesSpaHeatOnAckToWire)
{
	auto result = dispatcher->SetHeaterMode(BodyOfWaterIds::Spa, true);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// {state, devID} = SetOn 0x81, SPAHT 0x13.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x13));
}

BOOST_AUTO_TEST_CASE(SetHeaterMode_Solar_WritesSolarHeatAckToWire)
{
	// Shared body == the solar heater -> SOLHT (0x14).
	auto result = dispatcher->SetHeaterMode(BodyOfWaterIds::Shared, true);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// {state, devID} = SetOn 0x81, SOLHT 0x14.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x14));
}

//=============================================================================
// Aux/label/uuid-routed commands: ToggleByUuid / ToggleByLabel / CommandByUuid
// / CommandByLabel route through DispatchToCapable, which maps a hardware-aux
// device to QueueAuxToggleWrite -> a single setDev {state, devID} ACK on the next
// Status poll. (The emulated adapter is "running" from construction, so it passes
// ActuateDevice's IsRunning() deliverability gate.)
//=============================================================================

namespace
{
	// Add a hardware-aux device (with a JandyAuxillaryId) to the DataHub so the
	// uuid/label dispatch paths can find it and map it to an aux command.
	std::shared_ptr<AuxillaryDevice> AddAuxDevice(DataHub& data_hub, const std::string& label, Auxillaries::JandyAuxillaryIds aux_id)
	{
		using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;
		auto device = std::make_shared<AuxillaryDevice>();
		device->AuxillaryTraits.Set(LabelTrait{}, label);
		device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
		device->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, aux_id);
		data_hub.Devices.Add(device);
		return device;
	}
}

BOOST_AUTO_TEST_CASE(CommandByUuid_On_WritesAuxOnAckToWire)
{
	auto device = AddAuxDevice(*data_hub, "Pool Light", Auxillaries::JandyAuxillaryIds::Aux_1);

	auto result = dispatcher->CommandByUuid(device->Id(), ICommandDispatcher::DeviceAction::On);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// setDev {ack_type=state, data=devID}: Aux_1 (0x01+0x14=0x15) ON -> {0x81, 0x15}.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x15));
}

BOOST_AUTO_TEST_CASE(CommandByLabel_Off_WritesAuxOffAckToWire)
{
	AddAuxDevice(*data_hub, "Spa Light", Auxillaries::JandyAuxillaryIds::Aux_2);

	auto result = dispatcher->CommandByLabel("Spa Light", ICommandDispatcher::DeviceAction::Off);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// setDev {ack_type=state, data=devID}: Aux_2 (0x02+0x14=0x16) OFF -> {0x80, 0x16}.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x80, 0x16));
}

BOOST_AUTO_TEST_CASE(ToggleByUuid_OffDevice_WritesAuxOnAckToWire)
{
	// A device with no status trait toggles to On (SetOn) by default.
	auto device = AddAuxDevice(*data_hub, "Waterfall", Auxillaries::JandyAuxillaryIds::Aux_3);

	auto result = dispatcher->ToggleByUuid(device->Id());
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// setDev {ack_type=state, data=devID}: Aux_3 (0x03+0x14=0x17) default toggle => ON -> {0x81, 0x17}.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x81, 0x17));
}

BOOST_AUTO_TEST_CASE(ToggleByLabel_OnDevice_WritesAuxOffAckToWire)
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	// A device currently On toggles to Off (SetOff).
	auto device = AddAuxDevice(*data_hub, "Bubbler", Auxillaries::JandyAuxillaryIds::Aux_4);
	device->AuxillaryTraits.Set(AuxillaryStatusTrait{}, AuxillaryStatuses::On);

	auto result = dispatcher->ToggleByLabel("Bubbler");
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplaySerialAdapterStatus();

	// setDev {ack_type=state, data=devID}: Aux_4 (0x04+0x14=0x18) On => toggle to OFF -> {0x80, 0x18}.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x80, 0x18));
}

//=============================================================================
// IAQ-routed chlorinator commands.
//
// The chlorinator write is a page-GATED walk to the panel's own AquaPure page,
// which takes an ABSOLUTE output value -- the fast path, versus the OneTouch
// route's 5%-per-keypress stepping. It emits ONE command per IAQ_Poll and
// re-reads the rendered page id before choosing the next, so these tests drive
// the pages at it and assert the bytes that actually reach the wire.
//
// REGRESSION: this used to be four commands queued blindly at request time,
// assuming the AquaPure page opened from a FIXED page-button index. The master
// lays every page out from the installed equipment, so that index is not
// portable; and 0x02 is the panel's single "Menu / Back" key, whose meaning
// depends on the screen it is pressed from.
//=============================================================================

BOOST_AUTO_TEST_CASE(SetChlorinatorPercentage_WalksToAquaPureThenWritesAbsoluteValue)
{
	// Home: the only thing it may send is the Menu/Back key -- never a page-button guess.
	ReplayIAQPageWithButtons(IAQ_PAGE_HOME, { { 0, "Filter Pump" }, { 1, "Spa" } });

	auto result = dispatcher->SetChlorinatorPercentage(75, Kernel::BodyOfWaterIds::Pool);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplayIAQPoll();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x02));

	// Menu renders. It publishes NO buttons (confirmed on a live bus, iaq_chlorinator_set.cap:
	// PageStart 0x0f is followed straight by PageEnd), so this hop presses the confirmed key.
	ReplayIAQPageWithButtons(IAQ_PAGE_MENU, {});
	ClearWire();
	DrainIAQPollsUntilCommand();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x19));

	// AquaPure page renders: pick the Pool row by label, then submit.
	ReplayIAQPageWithButtons(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Spa 30%" }, { 2, "Quick Boost" }, { 3, "Boost Setup" } });
	ClearWire();
	DrainIAQPollsUntilCommand();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x11 + 0));

	ClearWire();
	DrainIAQPollsUntilCommand();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x80));

	// The master then opens the control-data window and the ABSOLUTE value goes across whole.
	ClearWire();
	ReplayIAQControlReady();
	BOOST_CHECK(Wire() == ExpectedControlDataWireBytes("175"));
}

BOOST_AUTO_TEST_CASE(SetChlorinatorPercentage_MenuWithoutAquaPure_NeverPressesAPageButton)
{
	// A menu that never opens the AquaPure page is proof the route does not exist here. The goal
	// is abandoned after a bounded number of attempts, and no value is ever submitted.
	ReplayIAQPageWithButtons(IAQ_PAGE_HOME, { { 0, "Filter Pump" } });

	auto result = dispatcher->SetChlorinatorPercentage(75, Kernel::BodyOfWaterIds::Pool);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ReplayIAQPageWithButtons(IAQ_PAGE_MENU, {});

	// It may press the AquaPure key, but staying on the menu means it never reaches 0x30, so no
	// row press and no value submit may ever reach the wire.
	const auto idle = ExpectedAckWireBytes(0x00, 0x00);
	const auto open_aquapure = ExpectedAckWireBytes(0x00, 0x19);
	for (int i = 0; i < 40; ++i)
	{
		ClearWire();
		ReplayIAQPoll();
		BOOST_CHECK((Wire() == idle) || (Wire() == open_aquapure));
	}

	ClearWire();
	ReplayIAQControlReady();
	BOOST_CHECK(Wire() != ExpectedControlDataWireBytes("175"));

	// And now that the route is known-absent, the dispatcher is told so up-front (this rig has
	// no OneTouch, so the refusal surfaces rather than falling back).
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->SetChlorinatorPercentage(75, Kernel::BodyOfWaterIds::Pool)),
		static_cast<int>(ICommandDispatcher::CommandResult::UnknownEquipmentType));
}

BOOST_AUTO_TEST_CASE(SetChlorinatorBoost_PressesTheBoostRowAndSubmitsNoValue)
{
	ReplayIAQPageWithButtons(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Spa 30%" }, { 2, "Quick Boost" }, { 3, "Boost Setup" } });

	auto result = dispatcher->SetChlorinatorBoost(true);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	DrainIAQPollsUntilCommand();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x11 + 2));

	// Boost is a toggle: no control-data value is armed.
	ClearWire();
	ReplayIAQControlReady();
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x00));
}

BOOST_AUTO_TEST_CASE(SelectIAQPageButton_WritesButtonSelectAckToWire)
{
	// SelectIAQPageButton(N) presses the on-screen PageButton at index N by emitting
	// command (0x11 + N) in the next IAQ_Poll ACK -- this drives the master's page UI
	// (navigate sub-pages / toggle equipment) exactly as a physical touch would.
	auto result = dispatcher->SelectIAQPageButton(2);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	ClearWire();
	ReplayIAQPoll();
	// IAQ ACK ack_type 0x00; button index 2 -> command 0x11 + 2 = 0x13.
	BOOST_CHECK(Wire() == ExpectedAckWireBytes(0x00, 0x13));
}

//-----------------------------------------------------------------------------
// Pending commands are one-shot: once the two-step setpoint write has emitted
// both the readySP (CMD_STATUS poll) and the setSP (DEV_READY poll), a further
// poll must NOT re-write either step.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SerialAdapterPendingCommand_IsConsumedAfterTwoStepWrite)
{
	auto result = dispatcher->SetPoolSetpoint(82);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	// Step 1 readySP {POOLSP 0x05, 0x35} on a CMD_STATUS poll.
	ClearWire();
	ReplaySerialAdapterStatus();
	auto readysp_bytes = ExpectedAckWireBytes(0x05, 0x35);
	BOOST_CHECK(Wire() == readysp_bytes);

	// Step 2 setSP {0x00, 82} on the DEV_READY poll.
	ClearWire();
	ReplaySerialAdapterDevReady(0x05);
	auto setsp_bytes = ExpectedAckWireBytes(0x00, 82);
	BOOST_CHECK(Wire() == setsp_bytes);

	// Both steps are now consumed: a further CMD_STATUS poll produces a
	// status-query ACK, neither the readySP nor the setSP bytes.
	ClearWire();
	ReplaySerialAdapterStatus();
	BOOST_REQUIRE(!Wire().empty());
	BOOST_CHECK(Wire() != readysp_bytes);
	BOOST_CHECK(Wire() != setsp_bytes);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// NON-EMULATED (real-hardware-only) rig: the controllers on the bus are real
// devices we only decode -- none is actively emulating, so NONE can transmit.
// A command must therefore (a) be reported as an honest failure by the
// dispatcher, and (b) put NO command bytes on the wire.  This is the end-to-end
// proof of the silent-no-op fix: before it, the real Serial Adapter (priority
// High) falsely returned Accepted and the dispatcher claimed Success while the
// ACK was swallowed downstream.
//=============================================================================

namespace
{
	struct CommandToWireNoEmulatorFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		CommandToWireNoEmulatorFixture()
			: data_hub(Find<DataHub>())
			, equipment_hub(Find<EquipmentHub>())
			, statistics_hub(Find<StatisticsHub>())
			, serial_impl(new Test::TestSerialPortImpl())
			, serial_port(std::make_shared<Serial::SerialPort>(
				std::unique_ptr<Test::TestSerialPortImpl>(serial_impl), *this))
			, dispatcher(std::make_shared<CommandDispatcher>(data_hub, equipment_hub))
		{
			serial_impl->EnableTestMode(true);
			Register<Interfaces::ICommandDispatcher>(dispatcher);

			Protocol::MessageGeneratorRegistry::Instance().Clear();
			Jandy::Protocol::RegisterMessageGenerator();

			protocol_task = std::make_shared<Protocol::ProtocolTask>(serial_port, statistics_hub);
			protocol_task->ConnectWriteSignal<Messages::JandyMessage_Ack>();
			protocol_task->ConnectWriteSignal<Messages::IAQMessage_ControlDataResponse>();

			// Register the controllers as REAL (is_emulated=false): they decode the bus
			// but never transmit.  The write signals ARE connected, so if anything WERE
			// emitted it would reach the wire -- proving the absence of bytes is real.
			auto sa_id = std::make_shared<JandyDeviceType>(JandyDeviceId(SERIAL_ADAPTER_ID));
			equipment_hub->AddDevice(std::make_unique<SerialAdapterDevice>(sa_id, *this, false));

			auto iaq_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_ID));
			equipment_hub->AddDevice(std::make_unique<IAQDevice>(iaq_id, *this, false));
		}

		~CommandToWireNoEmulatorFixture() override
		{
			protocol_task.reset();
			Protocol::MessageGeneratorRegistry::Instance().Clear();
			if (serial_impl != nullptr)
			{
				serial_impl->Reset();
			}
		}

		void Replay(const std::vector<uint8_t>& frame)
		{
			serial_impl->QueueReadData(frame);
			serial_impl->QueueReadData({}, boost::asio::error::would_block);
			(void)protocol_task->Poll();
		}

		void ReplaySerialAdapterStatus()
		{
			Replay(MakeFramedPacket(SERIAL_ADAPTER_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Status), { 0x00, 0x00, 0x00, 0x00, 0x00 }));
		}

		const std::vector<uint8_t>& Wire() const { return serial_impl->GetWrittenData(); }
		void ClearWire() { serial_impl->ClearWrittenData(); }

		std::shared_ptr<DataHub> data_hub;
		std::shared_ptr<EquipmentHub> equipment_hub;
		std::shared_ptr<StatisticsHub> statistics_hub;

		Test::TestSerialPortImpl* serial_impl;	// owned by serial_port
		std::shared_ptr<Serial::SerialPort> serial_port;
		std::shared_ptr<Protocol::ProtocolTask> protocol_task;
		std::shared_ptr<CommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_Integration_Flow_CommandToWire_NoEmulator, CommandToWireNoEmulatorFixture)

BOOST_AUTO_TEST_CASE(RealAdapterOnly_Toggle_ReportsFailureAndEmitsNoCommand)
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	// A hardware aux (with a JandyAuxillaryId) the dispatcher can resolve and route.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{ "Pool Light" });
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	device->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, Auxillaries::JandyAuxillaryIds::Aux_1);
	data_hub->Devices.Add(device);

	// No actively-emulating controller can transmit -> honest NoSerialAdapter, NOT a
	// false Success.
	auto result = dispatcher->CommandByUuid(device->Id(), ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));

	// And a subsequent Status poll must NOT carry the aux-on command bytes. A passive
	// adapter writes nothing at all, so the captured wire is empty (and certainly does
	// not equal the would-be Aux_1 ON command).
	ClearWire();
	ReplaySerialAdapterStatus();
	const auto aux_on_bytes = ExpectedAckWireBytes(0x81, 0x15);   // setDev {state, devID}: Aux_1 ON
	BOOST_CHECK(Wire() != aux_on_bytes);
}

BOOST_AUTO_TEST_SUITE_END()
