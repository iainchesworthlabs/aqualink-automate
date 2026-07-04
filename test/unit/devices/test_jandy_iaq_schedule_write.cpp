#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/messages/jandy_message_ack.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// iAQ controller-schedule WRITE (create a program). The emulated iAQ drives the
// AqualinkTouch Program pages: navigate to the Schedule list (0x28) -> Add
// Program (0x11) -> the device picker (0x38). RE'd from
// captures/iaq_schedule_{session,clean}.cap; see docs/iaq_schedule_protocol.md.
//
// This increment asserts the entry-point validation and the navigate -> Add
// Program emission, and that the writer fail-safes (abandons, no stray press) at
// the device-picker pick keystroke, which awaits a controlled picker capture.
//=============================================================================

namespace
{
	constexpr uint8_t IAQ_UI_ID = 0x33;
	constexpr uint8_t IAQ_PAGE_START = 0x23;
	constexpr uint8_t IAQ_PAGE_END = 0x28;
	constexpr uint8_t IAQ_TABLE_MESSAGE = 0x26;
	constexpr uint8_t IAQ_POLL = 0x30;
	constexpr uint8_t IAQ_PAGE_SCHEDULE_LIST = 0x28;
	constexpr uint8_t IAQ_PAGE_DEVICE_PICKER = 0x38;

	using WireFrame = std::vector<uint8_t>;

	WireFrame PageStart(uint8_t page_id) { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_PAGE_START, { page_id }); }
	WireFrame PageEnd() { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_PAGE_END, { 0x06, 0x0e }); }
	WireFrame Poll() { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_POLL, { 0x00 }); }

	// A group-0 device-picker row: [0x00][attr][ label 0x00 ]. attr 0 = the "Devices" header.
	WireFrame DeviceRow(uint8_t attr, const std::string& label)
	{
		std::vector<uint8_t> data{ 0x00, attr };
		for (char c : label) { data.push_back(static_cast<uint8_t>(c)); }
		data.push_back(0x00);
		return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_TABLE_MESSAGE, data);
	}

	Scheduling::ControllerSchedule ValidProgram(const std::string& target = "Pool Light")
	{
		Scheduling::ControllerSchedule p;
		p.target = target;
		p.days_of_week = 0x01;   // Monday (a single, controller-expressible day)
		p.on_hour = 9;  p.on_minute = 0;
		p.off_hour = 17; p.off_minute = 0;
		return p;
	}

	void ReplayPicker(Test::MockReplayHarness& harness, const std::vector<std::string>& devices)
	{
		std::vector<WireFrame> frames;
		frames.push_back(PageStart(IAQ_PAGE_DEVICE_PICKER));
		frames.push_back(DeviceRow(0, "Devices"));
		uint8_t attr = 1;
		for (const auto& d : devices) { frames.push_back(DeviceRow(attr++, d)); }
		frames.push_back(PageEnd());
		harness.Replay(frames);
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_IAQ_ScheduleWrite)

//--- entry-point validation -------------------------------------------------

BOOST_AUTO_TEST_CASE(Create_NotEmulated_IsNotSupported)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);

	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(Create_NotRepresentable_IsInvalidValue)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto arbitrary_days = ValidProgram();
	arbitrary_days.days_of_week = 0x15;   // Mon+Wed+Fri -- the controller cannot represent it
	BOOST_CHECK(device.CreateControllerProgram(arbitrary_days) == Capabilities::ActuationResult::InvalidValue);

	auto no_target = ValidProgram("");
	BOOST_CHECK(device.CreateControllerProgram(no_target) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Create_Valid_IsAccepted_ThenBusy)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::Accepted);
	// A second request while the first is in flight is rejected (busy panel -> NotSupported).
	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::NotSupported);
}

//--- closed-loop: full create (navigate -> add -> pick device -> set day) ----

namespace
{
	// Replay the schedule list carrying one program row "<target>?<on>?<off>?<days>".
	WireFrame ScheduleRow(uint8_t ordinal, const std::string& text)
	{
		std::vector<uint8_t> data{ 0x00, ordinal };
		for (char c : text)
		{
			data.push_back(c == '\t' ? static_cast<uint8_t>(0x09) : static_cast<uint8_t>(c));
		}
		data.push_back(0x00);
		return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_TABLE_MESSAGE, data);
	}
	void ReplayList(Test::MockReplayHarness& harness, const std::vector<std::string>& rows)
	{
		std::vector<WireFrame> frames;
		frames.push_back(PageStart(IAQ_PAGE_SCHEDULE_LIST));
		uint8_t ord = 1;
		for (const auto& r : rows) { frames.push_back(ScheduleRow(ord++, r)); }
		frames.push_back(PageEnd());
		harness.Replay(frames);
	}
}

BOOST_AUTO_TEST_CASE(Create_FullFlow_EmitsAdd_PickRow_Ok_Day_AndCompletes)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{
			if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); }
		});

	// Target Pool Light on Monday.
	auto program = ValidProgram("Pool Light");
	program.days_of_week = 0x01;   // Monday -> day key 0x18

	harness.Replay({ PageStart(IAQ_PAGE_SCHEDULE_LIST), PageEnd() });
	BOOST_REQUIRE(device.CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);

	// NavigateToList (already on 0x28) -> AddProgram emits 0x11.
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }

	// Device picker: Pool Light is the 3rd visible row -> click = 0x13 + 3 = 0x16, then OK = 0x13.
	ReplayPicker(harness, { "Filter Pump", "Spa", "Pool Light", "Spillway" });
	for (int i = 0; i < 12; ++i) { harness.Replay({ Poll() }); }

	// Back on the list with the freshly-created program (defaults + our day) -> day key 0x18, verify.
	ReplayList(harness, { "Pool Light\t1:00 PM\t1:00 PM\tM" });
	for (int i = 0; i < 12; ++i) { harness.Replay({ Poll() }); }

	// The emitted keypress sequence: Add, click row 3, OK, day = Monday.
	BOOST_REQUIRE_EQUAL(cmds.size(), 4u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[0]), 0x11);   // Add Program
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[1]), 0x16);   // click row 3 (Pool Light)
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[2]), 0x13);   // OK
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[3]), 0x18);   // day = Monday

	// The goal completed (Verify saw the program), so the panel is idle and a fresh request is accepted.
	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Create_ScrollsPicker_WhenDeviceNotOnFirstPage)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{
			if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); }
		});

	auto program = ValidProgram("Clean Mode");
	program.days_of_week = 0x7f;   // all days -> day key 0x17

	harness.Replay({ PageStart(IAQ_PAGE_SCHEDULE_LIST), PageEnd() });
	BOOST_REQUIRE(device.CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }   // -> Add (0x11)

	// Page 1 does NOT contain Clean Mode -> the writer must scroll (0x12). Poll just enough for a
	// single scroll (a scroll is issued, then IAQ_SCHEDULE_SETTLE_POLLS dwell) before page 2 arrives.
	ReplayPicker(harness, { "Filter Pump", "Spa", "Pool Heat", "Spa Heat" });
	for (int i = 0; i < 3; ++i) { harness.Replay({ Poll() }); }
	BOOST_REQUIRE_GE(cmds.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[0]), 0x11);   // Add
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[1]), 0x12);   // scroll the picker

	// Page 2 has Clean Mode at row 2 -> click = 0x13 + 2 = 0x15, then OK.
	ReplayPicker(harness, { "Swim Jet", "Clean Mode", "Air Blower" });
	for (int i = 0; i < 12; ++i) { harness.Replay({ Poll() }); }
	ReplayList(harness, { "Clean Mode\t1:00 PM\t1:00 PM\tAll" });
	for (int i = 0; i < 12; ++i) { harness.Replay({ Poll() }); }

	// Full sequence: Add, scroll, click row 2, OK, day = All.
	BOOST_REQUIRE_EQUAL(cmds.size(), 5u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[2]), 0x15);   // click row 2 (Clean Mode on page 2)
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[3]), 0x13);   // OK
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[4]), 0x17);   // day = All
}

BOOST_AUTO_TEST_SUITE_END()
