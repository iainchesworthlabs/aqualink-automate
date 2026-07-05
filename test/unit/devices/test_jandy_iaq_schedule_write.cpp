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
#include "jandy/messages/iaq/iaq_message_control_data_response.h"

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
	constexpr uint8_t IAQ_PAGE_MESSAGE = 0x25;
	constexpr uint8_t IAQ_CONTROL_READY = 0x31;
	constexpr uint8_t IAQ_PAGE_TIME_PICKER = 0x29;

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

	// A PageMessage line: [line_id][ ascii 0x00 ].
	WireFrame PageMessage(uint8_t line_id, const std::string& text)
	{
		std::vector<uint8_t> data{ line_id };
		for (char c : text) { data.push_back(static_cast<uint8_t>(c)); }
		data.push_back(0x00);
		return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_PAGE_MESSAGE, data);
	}
	// The time picker (0x29): line 1 = HH:MM, line 2 = AM/PM (what the writer reads).
	void ReplayTimePicker(Test::MockReplayHarness& harness, const std::string& meridiem)
	{
		harness.Replay({ PageStart(IAQ_PAGE_TIME_PICKER), PageMessage(1, "01:00"), PageMessage(2, meridiem), PageEnd() });
	}
	// The master's control-ready prompt that follows a 0x80 submit; it makes the iAQ send its value.
	void ReplayControlReady(Test::MockReplayHarness& harness)
	{
		harness.Replay({ Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_CONTROL_READY, { 0x00 }) });
	}
}

BOOST_AUTO_TEST_CASE(Create_FullFlow_Add_Pick_Times_Day_AndCompletes)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{ if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); } });

	// Capture the value-submit responses (the "1"+HH:MM the iAQ sends after each 0x80).
	std::vector<std::string> submits;
	boost::signals2::scoped_connection conn2 = Messages::IAQMessage_ControlDataResponse::GetPublisher()->connect(
		[&submits](std::reference_wrapper<const Messages::IAQMessage_ControlDataResponse> r)
		{ submits.push_back(r.get().ToString()); });

	auto poll_until = [&](std::size_t n)
	{
		for (int i = 0; (i < 40) && (cmds.size() < n); ++i) { harness.Replay({ Poll() }); }
	};

	// Pool Light, ON 9:00 AM, OFF 5:00 PM (17:00), Monday.
	auto program = ValidProgram("Pool Light");
	program.on_hour = 9;   program.on_minute = 0;
	program.off_hour = 17; program.off_minute = 0;
	program.days_of_week = 0x01;   // Monday -> day key 0x18

	harness.Replay({ PageStart(IAQ_PAGE_SCHEDULE_LIST), PageEnd() });
	BOOST_REQUIRE(device.CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);

	poll_until(1);                                                    // Add Program
	ReplayPicker(harness, { "Filter Pump", "Spa", "Pool Light" });    // Pool Light = row 3
	poll_until(3);                                                    // click row 3, OK
	ReplayList(harness, { "Pool Light\t1:00 PM\t1:00 PM\tAll" });     // new program (defaults)
	poll_until(4);                                                    // open ON field (0x21)
	ReplayTimePicker(harness, "PM");                                  // picker defaults to PM; want AM
	poll_until(5);                                                    // AM/PM toggle (0x11)
	ReplayTimePicker(harness, "AM");
	poll_until(6);                                                    // submit ON (0x80)
	ReplayControlReady(harness);                                      // -> sends "109:00"
	ReplayList(harness, { "Pool Light\t9:00 AM\t1:00 PM\tAll" });
	poll_until(7);                                                    // open OFF field (0x22)
	ReplayTimePicker(harness, "PM");                                  // want PM (17:00) -> no toggle
	poll_until(8);                                                    // submit OFF (0x80)
	ReplayControlReady(harness);                                      // -> sends "105:00"
	ReplayList(harness, { "Pool Light\t9:00 AM\t5:00 PM\tAll" });
	poll_until(9);                                                    // day = Monday (0x18)
	ReplayList(harness, { "Pool Light\t9:00 AM\t5:00 PM\tM" });       // final program
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }       // Verify -> Done

	const std::vector<uint8_t> expected{ 0x11, 0x16, 0x13, 0x21, 0x11, 0x80, 0x22, 0x80, 0x18 };
	BOOST_REQUIRE_EQUAL(cmds.size(), expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(cmds[i]), static_cast<int>(expected[i]));
	}

	// The two time submits carried the 12-hour values (ON 9:00 AM, OFF 5:00 PM) as "1"+HH:MM.
	BOOST_REQUIRE_EQUAL(submits.size(), 2u);
	BOOST_CHECK(submits[0].find("109:00") != std::string::npos);
	BOOST_CHECK(submits[1].find("105:00") != std::string::npos);

	// Verify saw the fully-configured program, so the goal completed and the panel is idle again.
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
		{ if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); } });

	auto program = ValidProgram("Clean Mode");

	harness.Replay({ PageStart(IAQ_PAGE_SCHEDULE_LIST), PageEnd() });
	BOOST_REQUIRE(device.CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }   // -> Add (0x11)

	// Page 1 does NOT contain Clean Mode -> the writer must scroll (0x12). Poll just enough for a
	// single scroll (issued, then IAQ_SCHEDULE_SETTLE_POLLS dwell) before page 2 arrives.
	ReplayPicker(harness, { "Filter Pump", "Spa", "Pool Heat", "Spa Heat" });
	for (int i = 0; i < 3; ++i) { harness.Replay({ Poll() }); }
	BOOST_REQUIRE_GE(cmds.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[0]), 0x11);   // Add
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[1]), 0x12);   // scroll the picker

	// Page 2 has Clean Mode at row 2 -> click = 0x13 + 2 = 0x15, then OK (0x13). (The flow then
	// proceeds to the time fields, exercised by the full-flow test above.)
	ReplayPicker(harness, { "Swim Jet", "Clean Mode", "Air Blower" });
	for (int i = 0; i < 12; ++i) { harness.Replay({ Poll() }); }

	BOOST_REQUIRE_GE(cmds.size(), 4u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[2]), 0x15);   // click row 2 (Clean Mode on page 2)
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[3]), 0x13);   // OK
}

//--- delete an existing program ---------------------------------------------

BOOST_AUTO_TEST_CASE(Delete_NotEmulated_IsNotSupported_EmptyTarget_IsInvalid)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice passive(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	BOOST_CHECK(passive.DeleteControllerProgram(ValidProgram("Pool Heat")) == Capabilities::ActuationResult::NotSupported);

	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	BOOST_CHECK(device.DeleteControllerProgram(ValidProgram("")) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Delete_ClicksRow_Delete_Ok_AndCompletes)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{ if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); } });

	auto poll_until = [&](std::size_t n)
	{ for (int i = 0; (i < 40) && (cmds.size() < n); ++i) { harness.Replay({ Poll() }); } };

	// Delete Pool Heat (row 2): all days, 11:00 AM -> 2:00 PM (matches the list row below).
	Scheduling::ControllerSchedule target;
	target.target = "Pool Heat"; target.days_of_week = 0x7f;
	target.on_hour = 11; target.on_minute = 0; target.off_hour = 14; target.off_minute = 0;

	ReplayList(harness, { "Filter Pump\t11:00 AM\t2:00 PM\tAll", "Pool Heat\t11:00 AM\t2:00 PM\tAll" });
	BOOST_REQUIRE(device.DeleteControllerProgram(target) == Capabilities::ActuationResult::Accepted);

	poll_until(3);   // SelectRow (click row 2 = 0x24) -> Delete (0x13) -> Ok (0x01)

	// The program is gone: re-render the list without it -> VerifyGone completes.
	ReplayList(harness, { "Filter Pump\t11:00 AM\t2:00 PM\tAll" });
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }

	BOOST_REQUIRE_EQUAL(cmds.size(), 3u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[0]), 0x24);   // click program row 2 (0x22 + 2)
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[1]), 0x13);   // Delete
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[2]), 0x01);   // Ok (confirm)

	// Goal completed -> panel idle, a fresh request is accepted.
	BOOST_CHECK(device.DeleteControllerProgram(target) == Capabilities::ActuationResult::Accepted);
}

//--- edit an existing program -----------------------------------------------

BOOST_AUTO_TEST_CASE(Edit_NotEmulated_IsNotSupported_NotRepresentable_IsInvalid)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));

	IAQDevice passive(id, harness.HubLocatorRef(), /*is_emulated=*/false);
	BOOST_CHECK(passive.EditControllerProgram(ValidProgram("Pool Heat"), ValidProgram("Pool Heat")) == Capabilities::ActuationResult::NotSupported);

	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);
	// Empty existing target -> InvalidValue.
	BOOST_CHECK(device.EditControllerProgram(ValidProgram(""), ValidProgram("Pool Heat")) == Capabilities::ActuationResult::InvalidValue);
	// A desired program the controller cannot represent (Mon+Wed+Fri) -> InvalidValue.
	auto bad_desired = ValidProgram("Pool Heat");
	bad_desired.days_of_week = 0x15;
	BOOST_CHECK(device.EditControllerProgram(ValidProgram("Pool Heat"), bad_desired) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Edit_ClicksRow_Edit_SetsTimesAndDay_AndCompletes)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{ if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); } });

	std::vector<std::string> submits;
	boost::signals2::scoped_connection conn2 = Messages::IAQMessage_ControlDataResponse::GetPublisher()->connect(
		[&submits](std::reference_wrapper<const Messages::IAQMessage_ControlDataResponse> r)
		{ submits.push_back(r.get().ToString()); });

	auto poll_until = [&](std::size_t n)
	{ for (int i = 0; (i < 40) && (cmds.size() < n); ++i) { harness.Replay({ Poll() }); } };

	// Existing: Pool Light, ON 9:00 AM, OFF 5:00 PM, Monday (row 2 in the list below).
	Scheduling::ControllerSchedule existing;
	existing.target = "Pool Light"; existing.days_of_week = 0x01;
	existing.on_hour = 9; existing.on_minute = 0; existing.off_hour = 17; existing.off_minute = 0;

	// Desired: same device, ON 10:00 AM, OFF 6:00 PM (18:00), Wednesday (day key 0x1a).
	Scheduling::ControllerSchedule desired;
	desired.target = "Pool Light"; desired.days_of_week = 0x04;   // Wednesday -> 0x18 + 2 = 0x1a
	desired.on_hour = 10; desired.on_minute = 0; desired.off_hour = 18; desired.off_minute = 0;

	// The list shows the existing program at row 2 (row-click = 0x22 + 2 = 0x24).
	ReplayList(harness, { "Filter Pump\t8:00 AM\t9:00 AM\tAll", "Pool Light\t9:00 AM\t5:00 PM\tM" });
	BOOST_REQUIRE(device.EditControllerProgram(existing, desired) == Capabilities::ActuationResult::Accepted);

	poll_until(2);                                                    // click row 2 (0x24) -> Edit (0x12)
	ReplayList(harness, { "Filter Pump\t8:00 AM\t9:00 AM\tAll", "Pool Light\t9:00 AM\t5:00 PM\tM" });
	poll_until(3);                                                    // open ON field (0x21)
	ReplayTimePicker(harness, "PM");                                  // picker defaults PM; want AM (10:00)
	poll_until(4);                                                    // AM/PM toggle (0x11)
	ReplayTimePicker(harness, "AM");
	poll_until(5);                                                    // submit ON (0x80)
	ReplayControlReady(harness);                                      // -> sends "110:00"
	ReplayList(harness, { "Filter Pump\t8:00 AM\t9:00 AM\tAll", "Pool Light\t10:00 AM\t5:00 PM\tM" });
	poll_until(6);                                                    // open OFF field (0x22)
	ReplayTimePicker(harness, "PM");                                  // want PM (18:00) -> no toggle
	poll_until(7);                                                    // submit OFF (0x80)
	ReplayControlReady(harness);                                      // -> sends "106:00"
	ReplayList(harness, { "Filter Pump\t8:00 AM\t9:00 AM\tAll", "Pool Light\t10:00 AM\t6:00 PM\tM" });
	poll_until(8);                                                    // day = Wednesday (0x1a)
	ReplayList(harness, { "Filter Pump\t8:00 AM\t9:00 AM\tAll", "Pool Light\t10:00 AM\t6:00 PM\tW" });   // desired
	for (int i = 0; i < 8; ++i) { harness.Replay({ Poll() }); }       // Verify -> Done

	const std::vector<uint8_t> expected{ 0x24, 0x12, 0x21, 0x11, 0x80, 0x22, 0x80, 0x1a };
	BOOST_REQUIRE_EQUAL(cmds.size(), expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(cmds[i]), static_cast<int>(expected[i]));
	}

	// The two time submits carried the desired 12-hour values (ON 10:00 AM, OFF 6:00 PM).
	BOOST_REQUIRE_EQUAL(submits.size(), 2u);
	BOOST_CHECK(submits[0].find("110:00") != std::string::npos);
	BOOST_CHECK(submits[1].find("106:00") != std::string::npos);

	// Verify saw the desired program -> goal completed, panel idle again.
	BOOST_CHECK(device.EditControllerProgram(existing, desired) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_SUITE_END()
