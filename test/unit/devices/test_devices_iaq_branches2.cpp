#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_ids.h"

#include "jandy/auxillaries/jandy_auxillary_id.h"

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/preferences_hub.h"
#include "kernel/system_boards.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// IAQDevice message + status processing -- the arms the existing suites do not
// reach: the "seen but otherwise inert" master frames (PageContinue, StartUp,
// MessageLong, CommandReady, OneTouchStatus), the out-of-range display-line
// guards, the button-table erase, the schedule-row reject, the legacy
// (sentinel) MainStatus setpoint path, the AuxStatus presence gates, and the
// DeviceActuator's already-in-state / unresolvable-target arms.
//=============================================================================

namespace
{
	constexpr uint8_t IAQ_DEVICE_ID{ 0x33 };

	constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
	constexpr uint8_t IAQ_PAGE_SCHEDULE_LIST{ 0x28 };

	// The fixed status page is 18 lines: line ids 18 and above are out of range.
	constexpr uint8_t IAQ_STATUS_PAGE_LINES{ 18 };
	constexpr uint8_t IAQ_MESSAGE_TABLE_LINES{ 18 };

	using Frame = std::vector<uint8_t>;
	using MessageIds = Messages::JandyMessageIds;

	Frame Build(MessageIds id, const std::vector<uint8_t>& payload)
	{
		return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, static_cast<uint8_t>(id), payload);
	}

	std::vector<uint8_t> AsBytes(const std::string& text)
	{
		std::vector<uint8_t> bytes;
		for (const char c : text) { bytes.push_back(static_cast<uint8_t>(c)); }
		return bytes;
	}

	Frame PageStart(uint8_t page_id) { return Build(MessageIds::IAQ_PageStart, { page_id }); }
	Frame PageEnd() { return Build(MessageIds::IAQ_PageEnd, { 0x06, 0x0e }); }
	Frame Poll() { return Build(MessageIds::IAQ_Poll, { 0x00 }); }

	Frame PageMessage(uint8_t line_id, const std::string& text)
	{
		std::vector<uint8_t> payload{ line_id };
		for (const auto byte : AsBytes(text)) { payload.push_back(byte); }
		return Build(MessageIds::IAQ_PageMessage, payload);
	}

	Frame TableMessage(uint8_t line_id, uint8_t attribute, const std::string& text)
	{
		std::vector<uint8_t> payload{ line_id, attribute };
		for (const auto byte : AsBytes(text)) { payload.push_back(byte); }
		payload.push_back(0x00);
		return Build(MessageIds::IAQ_TableMessage, payload);
	}

	Frame TitleMessage(const std::string& title)
	{
		return Build(MessageIds::IAQ_TitleMessage, AsBytes(title));
	}

	// PageButton payload: index, state, (unused), type, name...
	Frame PageButton(uint8_t index, uint8_t state, const std::string& name)
	{
		std::vector<uint8_t> payload{ index, state, 0x00, 0x00 };
		for (const auto byte : AsBytes(name)) { payload.push_back(byte); }
		return Build(MessageIds::IAQ_PageButton, payload);
	}

	// AuxStatus payload: count, indices..., then per device: state, type, pad, pad, name_len, name.
	Frame AuxStatus(const std::vector<std::tuple<uint8_t, bool, std::string>>& devices)
	{
		std::vector<uint8_t> payload;
		payload.push_back(static_cast<uint8_t>(devices.size()));
		for (const auto& [index, is_on, name] : devices) { (void)is_on; (void)name; payload.push_back(index); }
		for (const auto& [index, is_on, name] : devices)
		{
			(void)index;
			payload.push_back(is_on ? 0x01 : 0x00);
			payload.push_back(0x00);   // type
			payload.push_back(0x00);   // pad
			payload.push_back(0x00);   // pad
			payload.push_back(static_cast<uint8_t>(name.size()));
			for (const auto byte : AsBytes(name)) { payload.push_back(byte); }
		}
		return Build(MessageIds::IAQ_AuxStatus, payload);
	}

	void PushTempBE(std::vector<uint8_t>& payload, uint16_t raw)
	{
		payload.push_back(static_cast<uint8_t>(raw >> 8));
		payload.push_back(static_cast<uint8_t>(raw & 0xFF));
	}

	// Current (no-sentinel) MainStatus: device ids, flags, pool/spa targets + air + water.
	Frame MainStatus_Current(bool pump_on = true)
	{
		std::vector<uint8_t> payload{ 0x03, 0x01, 0x02, 0x08 };
		payload.push_back(pump_on ? 0x01 : 0x00);
		payload.push_back(0x01);   // pool heater = Heating
		payload.push_back(0x00);   // pool mode
		payload.push_back(0x00);   // spa heater
		payload.push_back(0x00);   // solar
		PushTempBE(payload, 28);   // pool target
		PushTempBE(payload, 32);   // spa target
		PushTempBE(payload, 24);   // air
		PushTempBE(payload, 27);   // water
		return Build(MessageIds::IAQ_MainStatus, payload);
	}

	// LEGACY (sentinel 0x0e 0x0f) MainStatus: only the ACTIVE body's target is on the wire, as
	// a trailing heater setpoint -- there are no separate pool/spa setpoint fields. Temperatures
	// are big-endian deci-Celsius.
	Frame MainStatus_Legacy(bool spa_mode)
	{
		std::vector<uint8_t> payload{ 0x01, 0x01, 0x0e, 0x0f };
		payload.push_back(0x01);                   // pump on
		payload.push_back(spa_mode ? 0x01 : 0x00); // spa mode
		payload.push_back(0x00);                   // unknown
		payload.push_back(0x00);                   // unknown
		PushTempBE(payload, 270);                  // pool  27.0C
		PushTempBE(payload, 350);                  // spa   35.0C
		PushTempBE(payload, 240);                  // air   24.0C
		PushTempBE(payload, 300);                  // heater setpoint 30.0C
		return Build(MessageIds::IAQ_MainStatus, payload);
	}

	// The watchdog hooks are protected: expose them so a test can settle the start-up state
	// machine without waiting on wall-clock time (mirrors the seam in test_devices_iaq.cpp).
	struct SeamedIAQDevice : public IAQDevice
	{
		using IAQDevice::IAQDevice;
		void TriggerWatchdogTimeout() { WatchdogTimeoutOccurred(); }
	};

	std::shared_ptr<JandyDeviceType> MakeDeviceId()
	{
		return std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	}

	std::string JoinScreenLines(const IAQDevice& device)
	{
		std::string joined;
		for (const auto& line : device.DescribeDiagnostics()["screen"]["lines"])
		{
			joined += line.get<std::string>();
			joined += '\n';
		}
		return joined;
	}

	std::string PendingCommand(const IAQDevice& device)
	{
		return device.DescribeDiagnostics()["pending_command"].get<std::string>();
	}

	std::shared_ptr<Kernel::AuxillaryDevice> MakeLabelledAux(const std::string& label)
	{
		auto aux = std::make_shared<Kernel::AuxillaryDevice>();
		aux->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, label);
		return aux;
	}
}
// unnamed namespace

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQDevice_MessageBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(InertMasterFrames_AreStillTreatedAsTrafficAddressedToThisId)
{
	// PageContinue / StartUp / MessageLong / CommandReady / OneTouchStatus carry nothing the
	// device decodes today, but each IS traffic addressed to this id -- so a later watchdog
	// timeout must read as a genuine drop-out (Faulted), not "the id was never on the bus"
	// (NotPresent). Getting that wrong is what makes a live panel report a phantom absence.
	const std::vector<std::pair<MessageIds, const char*>> inert_frames{
		{ MessageIds::IAQ_PageContinue,   "PageContinue" },
		{ MessageIds::IAQ_StartUp,        "StartUp" },
		{ MessageIds::IAQ_MessageLong,    "MessageLong" },
		{ MessageIds::IAQ_CommandReady,   "CommandReady" },
		{ MessageIds::IAQ_OneTouchStatus, "OneTouchStatus" },
		{ MessageIds::IAQ_ControlReady,   "ControlReady" }
	};

	for (const auto& [message_id, name] : inert_frames)
	{
		BOOST_TEST_CONTEXT(name)
		{
			Test::MockReplayHarness harness;
			SeamedIAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

			harness.Replay(Build(message_id, { 0x00, 0x01, 0x02, 0x03, 0x04 }));
			device.TriggerWatchdogTimeout();

			BOOST_CHECK(device.IsFaulted());
			BOOST_CHECK(!device.IsNotPresent());
		}
	}
}

BOOST_AUTO_TEST_CASE(PageAndTableMessages_ForOutOfRangeLines_AreDroppedNotIndexed)
{
	// The decoded status page and the table accumulator are FIXED 18-line buffers. A master line
	// id beyond them must be logged and discarded; indexing it would run off the end. Decoding
	// must also carry on normally afterwards rather than being left in a broken state.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);

	BOOST_CHECK_NO_THROW(harness.Replay({
		PageStart(IAQ_PAGE_HOME),
		PageMessage(0, "In Range Line"),
		PageMessage(IAQ_STATUS_PAGE_LINES, "Out Of Range Line"),
		PageMessage(static_cast<uint8_t>(IAQ_STATUS_PAGE_LINES + 40), "Way Out Of Range"),
		TableMessage(IAQ_MESSAGE_TABLE_LINES, 1, "Out Of Range Table Row"),
		TableMessage(200, 2, "Also Out Of Range"),
		PageEnd()
	}));

	// A well-formed Schedule page decoded straight afterwards still publishes, proving the guard
	// dropped those frames rather than corrupting the page decoder.
	auto store = harness.HubLocatorRef().Find<Scheduling::ControllerScheduleStore>();
	BOOST_REQUIRE(nullptr != store);

	harness.Replay({
		PageStart(IAQ_PAGE_SCHEDULE_LIST),
		TitleMessage("Schedule Group A"),
		TableMessage(0, 1, "Filter Pump	11:00 AM	2:00 PM	All"),
		PageEnd()
	});

	BOOST_REQUIRE_EQUAL(store->List().size(), 1u);
	BOOST_CHECK_EQUAL(store->List()[0].target, "Filter Pump");
}

BOOST_AUTO_TEST_CASE(PageButton_WithABlankName_ClearsThatSlot)
{
	// Button indices are reused as the page's device list changes, so a blank name must ERASE
	// the slot: a stale name left behind would let the actuator press the wrong button.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	harness.Replay({ PageStart(IAQ_PAGE_HOME), PageButton(4, 0x00, "Pool Light") });

	// While the button is on screen the actuator can resolve it.
	auto aux = MakeLabelledAux("Pool Light");
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);

	// The master re-pushes the slot with a blank name -> the entry is gone, so the same request
	// can no longer be mapped and the dispatcher falls back to another controller.
	harness.Replay({ PageButton(4, 0x00, "   ") });
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::MappingFailed);
}

BOOST_AUTO_TEST_CASE(SchedulePage_SkipsRowsThatAreNotProgramEntries)
{
	// The Schedule list page id can exist on other models; the row parser is the gate, so a row
	// that is not a program entry is skipped rather than published as a garbage schedule.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);
	auto store = harness.HubLocatorRef().Find<Scheduling::ControllerScheduleStore>();
	BOOST_REQUIRE(nullptr != store);

	harness.Replay({
		PageStart(IAQ_PAGE_SCHEDULE_LIST),
		TitleMessage("Schedule Group B"),
		TableMessage(0, 1, "Filter Pump\t11:00 AM\t2:00 PM\tAll"),
		TableMessage(0, 2, "Press Add to create a program"),
		TableMessage(0, 3, "Pool Light\t9:00 AM\t5:00 PM\tWkends"),
		PageEnd()
	});

	BOOST_CHECK(store->Status() == Scheduling::ControllerScheduleStatus::Available);
	BOOST_CHECK_EQUAL(store->ActiveGroup(), "B");
	BOOST_REQUIRE_EQUAL(store->List().size(), 2u);
	BOOST_CHECK_EQUAL(store->List()[0].target, "Filter Pump");
	BOOST_CHECK_EQUAL(store->List()[1].target, "Pool Light");
	// The group is stripped from the title and folded into the stable per-slot id.
	BOOST_CHECK_EQUAL(store->List()[0].group, "B");
	BOOST_CHECK_EQUAL(store->List()[0].id, std::string("iaq-B-1"));
	BOOST_CHECK_EQUAL(store->List()[1].id, std::string("iaq-B-3"));
	BOOST_CHECK_EQUAL(store->List()[1].name, std::string("Pool Light"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQDevice_StatusBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(LegacyMainStatus_AppliesTheSetpointToTheActiveBodyOnly)
{
	// The legacy (sentinel) wire format carries ONE target -- the active body's. Writing it to
	// the wrong body would silently corrupt the other body's setpoint.
	{
		Test::MockReplayHarness harness;
		IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);
		harness.Replay(MainStatus_Legacy(/*spa_mode=*/false));

		auto hub = harness.DataHub();
		BOOST_REQUIRE(hub->PoolTempSetpoint().has_value());
		BOOST_CHECK_CLOSE(hub->PoolTempSetpoint()->InCelsius().value(), 30.0, 0.5);
		BOOST_CHECK(!hub->SpaTempSetpoint().has_value());
	}
	{
		Test::MockReplayHarness harness;
		IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);
		harness.Replay(MainStatus_Legacy(/*spa_mode=*/true));

		auto hub = harness.DataHub();
		BOOST_REQUIRE(hub->SpaTempSetpoint().has_value());
		BOOST_CHECK_CLOSE(hub->SpaTempSetpoint()->InCelsius().value(), 30.0, 0.5);
		BOOST_CHECK(!hub->PoolTempSetpoint().has_value());
	}
}

BOOST_AUTO_TEST_CASE(StatusScreen_SummarisesTheAuxOnOffStateFromTheDataHub)
{
	// The rendered System Status page appends one line per known aux. It reads them from the
	// DataHub (kept fresh by AuxStatus), so an aux decoded before the MainStatus shows up there.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);

	harness.Replay(AuxStatus({ { 0x01, true, "Pool Light" }, { 0x02, false, "Spa Blower" } }));
	harness.Replay(MainStatus_Current());

	const auto joined = JoinScreenLines(device);
	BOOST_CHECK(joined.find("Pool Light: On") != std::string::npos);
	BOOST_CHECK(joined.find("Spa Blower: Off") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AuxStatus_AnUnknownDeviceIndex_IsSkipped)
{
	// The master can report an index that is not a known aux relay; it must be skipped rather
	// than turned into a device with a fabricated identity.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);

	harness.Replay(AuxStatus({ { 0x7e, true, "Mystery Relay" }, { 0x01, true, "Pool Light" } }));

	auto hub = harness.DataHub();
	BOOST_CHECK_EQUAL(hub->Auxillaries().size(), 1u);
	BOOST_CHECK(nullptr != hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_1)));
}

BOOST_AUTO_TEST_CASE(AuxStatus_AnAuxTheDecodedModelCannotHave_IsNotCreated)
{
	// A status reply is not evidence a relay exists. Once the panel model is known, an aux that
	// belongs to a power centre the model does not have must be ignored -- this is what stops
	// phantom "Aux B1"-style devices appearing on a single-centre panel.
	Test::MockReplayHarness harness;
	auto hub = harness.DataHub();
	hub->SystemBoard = Kernel::SystemBoards::RS8_Combo;
	hub->ExpectedPowerCenterCount = 1;
	hub->ExpectedAuxillaryCount = 7;

	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);

	harness.Replay(AuxStatus({ { 0x08, true, "Phantom B1" }, { 0x01, true, "Pool Light" } }));

	BOOST_CHECK(nullptr == hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_B1)));
	BOOST_CHECK(nullptr != hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_1)));
}

BOOST_AUTO_TEST_CASE(AuxStatus_AnOperatorForcedAbsentAux_IsRemovedAndStaysRemoved)
{
	// An operator override that says "this slot does not exist" must survive the next wire
	// event, otherwise it flip-flops back every time the panel reports the aux.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);
	auto hub = harness.DataHub();

	// First report creates the device.
	harness.Replay(AuxStatus({ { 0x02, true, "Ghost Aux" } }));
	BOOST_REQUIRE(nullptr != hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_2)));

	// The operator forces it absent; the next report removes it instead of refreshing it.
	auto preferences = harness.HubLocatorRef().Find<Kernel::PreferencesHub>();
	BOOST_REQUIRE(nullptr != preferences);
	preferences->AuxPresenceOverrides = nlohmann::json{ { "Aux2", "absent" } };

	harness.Replay(AuxStatus({ { 0x02, true, "Ghost Aux" } }));
	BOOST_CHECK(nullptr == hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_2)));

	// And it is not recreated by a further report.
	harness.Replay(AuxStatus({ { 0x02, false, "Ghost Aux" } }));
	BOOST_CHECK(nullptr == hub->Devices.FindById(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_2)));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQDevice_ActuationBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(ActuateDevice_NullOrUnlabelledDevice_IsMappingFailed)
{
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_CHECK(device.ActuateDevice(nullptr, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::MappingFailed);

	auto no_label = std::make_shared<Kernel::AuxillaryDevice>();
	BOOST_CHECK(device.ActuateDevice(no_label, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::MappingFailed);

	// A whitespace-only label is no better than none: it can never match a button.
	auto blank_label = MakeLabelledAux("   ");
	BOOST_CHECK(device.ActuateDevice(blank_label, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::MappingFailed);
}

BOOST_AUTO_TEST_CASE(ActuateDevice_WhenPassive_IsNotSupported)
{
	// A non-emulated iAQ never transmits, so it must refuse rather than silently doing nothing.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/false);

	harness.Replay({ PageStart(IAQ_PAGE_HOME), PageButton(4, 0x00, "Pool Light") });
	BOOST_CHECK(device.ActuateDevice(MakeLabelledAux("Pool Light"), Capabilities::ActuationAction::On)
		== Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(ActuateDevice_AlreadyInTheRequestedState_IsANoOpNotAToggle)
{
	// The page button is a pure TOGGLE, so an explicit On against an already-ON button must NOT
	// be pressed -- doing so would turn the equipment off. Success is reported without a press.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	const uint8_t button_status_on = static_cast<uint8_t>(Messages::ButtonStatuses::On);
	harness.Replay({ PageStart(IAQ_PAGE_HOME), PageButton(4, button_status_on, "Pool Light") });

	auto aux = MakeLabelledAux("Pool Light");

	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::On) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x00"));   // nothing armed for the poll ACK

	// The opposite request DOES press the button (the state differs): 0x11 + index 4.
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Off) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x15"));
}

BOOST_AUTO_TEST_CASE(ActuateDevice_WithAnUnknownButtonState_PressesRatherThanAssuming)
{
	// An Unknown on-screen status carries no information, so the no-op shortcut must not apply:
	// the request is honoured with a press.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	harness.Replay({ PageStart(IAQ_PAGE_HOME), PageButton(4, 0xfe, "Pool Light") });   // 0xfe -> Unknown

	BOOST_CHECK(device.ActuateDevice(MakeLabelledAux("Pool Light"), Capabilities::ActuationAction::On)
		== Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x15"));
}

BOOST_AUTO_TEST_CASE(APendingCommandRidesTheNextPollAckAndIsThenCleared)
{
	// Commands can only leave on an IAQ_Poll ACK; a non-poll frame must leave the armed command
	// alone, and the poll must consume it exactly once.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	device.QueueCommand(0x19);
	BOOST_REQUIRE_EQUAL(PendingCommand(device), std::string("0x19"));

	harness.Replay(Build(MessageIds::IAQ_CommandReady, { 0x00 }));
	BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x19"));

	harness.Replay(Poll());
	BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x00"));
}

BOOST_AUTO_TEST_CASE(AChlorinatorGoalIsServicedOnlyOnAPoll)
{
	// The AquaPure write state machine rides the same poll-ACK channel: a queued goal must not
	// put anything on the wire until a poll arrives, and off an unrecognised page the first hop
	// is the panel's single Menu/Back key rather than a guessed page button.
	Test::MockReplayHarness harness;
	IAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> emitted;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&emitted](std::reference_wrapper<const Messages::JandyMessage_Ack> ack)
		{
			if (0x00 != ack.get().Command()) { emitted.push_back(ack.get().Command()); }
		});

	BOOST_REQUIRE(device.SetChlorinatorPercentage(60, Kernel::BodyOfWaterIds::Pool) == Capabilities::ActuationResult::Accepted);

	harness.Replay(Build(MessageIds::IAQ_CommandReady, { 0x00 }));
	BOOST_CHECK(emitted.empty());

	harness.Replay(Poll());
	BOOST_REQUIRE_EQUAL(emitted.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(emitted[0]), 0x02);
}

BOOST_AUTO_TEST_CASE(AfterTheWatchdogSettles_FurtherTrafficIsProcessedInThatState)
{
	// The operating-state machine must keep processing updates once it has left StartUp -- both
	// for an id that settled to NotPresent and for one that faulted after going silent.
	{
		Test::MockReplayHarness harness;
		SeamedIAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

		device.TriggerWatchdogTimeout();          // never addressed -> NotPresent
		BOOST_REQUIRE(device.IsNotPresent());

		harness.Replay(Poll());                   // late traffic is still processed, quietly
		BOOST_CHECK(device.IsNotPresent());
		BOOST_CHECK(!device.IsFaulted());
	}
	{
		Test::MockReplayHarness harness;
		SeamedIAQDevice device(MakeDeviceId(), harness.HubLocatorRef(), /*is_emulated=*/true);

		harness.Replay(Poll());                   // addressed...
		device.TriggerWatchdogTimeout();          // ...then silent -> Faulted
		BOOST_REQUIRE(device.IsFaulted());

		device.QueueCommand(0x19);
		harness.Replay(Poll());                   // still services the poll ACK while faulted
		BOOST_CHECK(device.IsFaulted());
		BOOST_CHECK_EQUAL(PendingCommand(device), std::string("0x00"));
	}
}

BOOST_AUTO_TEST_SUITE_END()
