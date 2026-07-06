#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/messages/jandy_message_ack.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/onetouch_test_device.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// OneTouch controller-schedule WRITE (ControllerScheduleWriter). The emulated
// OneTouch (0x40) drives its 16x12 character Program menu with discrete nav keys
// (Select / LineUp / LineDown / Back) to create / edit / delete one of the
// controller's own internal Program timers. RE'd from captures/onetouch_program.cap;
// see docs/onetouch_schedule_protocol.md (write path).
//
// These tests drive the REAL phase machine (ControllerScheduleWrite_ProcessStep)
// frame by frame through the screen seams (RenderScreenLineForTest presents a page;
// SetHighlightedLineForTest positions the cursor; DeliverStatusFrameForTest runs one
// ProcessControllerUpdates, which emits the queued key on the Status ACK) and assert
// the emitted KeyCommands stream + the closed-loop field entry.
//=============================================================================

namespace
{
	using KeyCommands = OneTouchDevice::KeyCommands;

	// The shared test-only subclass exposes the fault/screen/highlight/status drivers this suite needs.
	using WritableOneTouchDevice = Test::SeamedOneTouchDevice;

	struct OneTouchScheduleWriteFixture : public Test::HubLocatorInjector
	{
		OneTouchScheduleWriteFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x40)))
		{
		}

		std::shared_ptr<WritableOneTouchDevice> MakeDevice(bool emulated)
		{
			return std::make_shared<WritableOneTouchDevice>(device_type, *this, emulated);
		}

		// Present a full 12-line page (unspecified lines blanked) and set the cursor line.
		void RenderPage(WritableOneTouchDevice& dev, const std::vector<std::pair<uint8_t, std::string>>& lines, uint8_t highlight = 0xFF)
		{
			for (uint8_t i = 0; i < 12; ++i)
			{
				std::string text(16, ' ');
				for (const auto& [id, t] : lines)
				{
					if (id == i) { text = t; }
				}
				dev.RenderScreenLineForTest(i, text);
			}
			dev.SetHighlightedLineForTest(highlight);
		}

		// Drive the emulated device to NormalOperation: force a fault, then feed a recognised page
		// on sustained Status frames (the fault-recovery hysteresis threshold is 3).
		void DriveToNormalOperation(WritableOneTouchDevice& dev)
		{
			dev.ForceScrapingFaultedForTest();
			dev.RenderScreenLineForTest(9, "Equipment ON/OFF");
			dev.DeliverStatusFrameForTest();
			dev.DeliverStatusFrameForTest();
			dev.DeliverStatusFrameForTest();
			BOOST_REQUIRE(dev.IsInNormalOperationForTest());
		}

		std::shared_ptr<JandyDeviceType> device_type;
	};

	// The Program equipment LIST page: line 0 = "Program" title, then equipment rows.
	std::vector<std::pair<uint8_t, std::string>> ProgramListPage(const std::string& equipment, uint8_t row)
	{
		std::vector<std::pair<uint8_t, std::string>> lines{
			{ 0, "    Program     " },
			{ 1, "    Group A     " },
		};
		lines.emplace_back(row, equipment);
		return lines;
	}

	// A per-equipment Program DETAIL page (existing program present): "Pgm N of M" + ON/OFF/days +
	// the three action rows (Add 9 / Delete 10 / Change 11).
	std::vector<std::pair<uint8_t, std::string>> DetailPage(const std::string& equipment, const std::string& on, const std::string& off, const std::string& days)
	{
		return {
			{ 0, equipment },
			{ 2, "   Pgm 1 of 1   " },
			{ 3, on },
			{ 4, off },
			{ 5, days },
			{ 9, "Add      Program" },
			{ 10, "Delete   Program" },
			{ 11, "Change   Program" },
		};
	}

	// A per-equipment detail page with NO program: only the Add row + "No Programs".
	std::vector<std::pair<uint8_t, std::string>> EmptyDetailPage(const std::string& equipment)
	{
		return {
			{ 0, equipment },
			{ 4, "  No Programs   " },
			{ 9, "  Add Program   " },
		};
	}

	// The Add/Change editor page: title on line 1, ON on 3, OFF on 4, days on 5, arrow-keys prompt.
	std::vector<std::pair<uint8_t, std::string>> EditorPage(const std::string& equipment, const std::string& title, const std::string& on, const std::string& off, const std::string& days)
	{
		return {
			{ 0, equipment },
			{ 1, title },
			{ 3, on },
			{ 4, off },
			{ 5, days },
			{ 7, "Use Arrow Keys  " },
			{ 8, "to set value.   " },
			{ 9, "Then SELECT.    " },
		};
	}

	Scheduling::ControllerSchedule ProgramSpec(const std::string& target, int on_h, int on_m, int off_h, int off_m, uint8_t days)
	{
		Scheduling::ControllerSchedule p;
		p.target = target;
		p.on_hour = on_h; p.on_minute = on_m;
		p.off_hour = off_h; p.off_minute = off_m;
		p.days_of_week = days;
		return p;
	}
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_ScheduleWrite, OneTouchScheduleWriteFixture)

//--- entry-point gating -----------------------------------------------------

BOOST_AUTO_TEST_CASE(Create_NotEmulated_IsNotSupported)
{
	auto dev = MakeDevice(/*emulated*/ false);
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(Create_NotRepresentable_IsInvalidValue)
{
	auto dev = MakeDevice(/*emulated*/ true);

	// Mon+Wed+Fri (0x15) is an arbitrary combination the controller cannot represent.
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x15)) == Capabilities::ActuationResult::InvalidValue);
	// Empty target.
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Create_Valid_IsAccepted_ThenBusy)
{
	auto dev = MakeDevice(/*emulated*/ true);
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::Accepted);
	// A second request while the first is queued is rejected (one goal on the shared keypad).
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(Delete_NotEmulated_IsNotSupported_EmptyTarget_IsInvalid)
{
	auto passive = MakeDevice(/*emulated*/ false);
	BOOST_CHECK(passive->DeleteControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::NotSupported);

	auto dev = MakeDevice(/*emulated*/ true);
	BOOST_CHECK(dev->DeleteControllerProgram(ProgramSpec("", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Edit_NotEmulated_IsNotSupported_NotRepresentable_IsInvalid)
{
	auto passive = MakeDevice(/*emulated*/ false);
	BOOST_CHECK(passive->EditControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::NotSupported);

	auto dev = MakeDevice(/*emulated*/ true);
	// Empty existing target.
	BOOST_CHECK(dev->EditControllerProgram(ProgramSpec("", 11, 0, 14, 0, 0x7f), ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::InvalidValue);
	// Desired program the controller cannot represent (Mon+Wed+Fri).
	BOOST_CHECK(dev->EditControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), ProgramSpec("Pool Light", 11, 0, 14, 0, 0x15)) == Capabilities::ActuationResult::InvalidValue);
}

//--- full closed-loop flows -------------------------------------------------

namespace
{
	// Collect every non-NoKeyCommand key the device emits on the Status ACK (mirrors the IAQ write
	// test: the emulated device publishes its ACK via the JandyMessage_Ack send publisher).
	struct KeyRecorder
	{
		std::vector<uint8_t> keys;
		boost::signals2::scoped_connection conn;

		KeyRecorder()
		{
			conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
				[this](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
				{
					if (r.get().Command() != 0x00) { keys.push_back(r.get().Command()); }
				});
		}
	};

	constexpr uint8_t SELECT = static_cast<uint8_t>(KeyCommands::Select);     // 0x04
	constexpr uint8_t LINE_DOWN = static_cast<uint8_t>(KeyCommands::LineDown); // 0x05
	constexpr uint8_t LINE_UP = static_cast<uint8_t>(KeyCommands::LineUp);     // 0x06
}

BOOST_AUTO_TEST_CASE(Create_FullFlow_SetsTimesAndDays_AndCompletes)
{
	auto dev = MakeDevice(/*emulated*/ true);
	DriveToNormalOperation(*dev);

	KeyRecorder rec;

	// Pool Light, ON 11:00 AM, OFF 2:01 PM, All Days (the captured Add example).
	auto program = ProgramSpec("Pool Light", 11, 0, 14, 1, 0x7f);
	BOOST_REQUIRE(dev->CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);

	auto frame = [&]() { dev->DeliverStatusFrameForTest(); };
	auto render_editor = [&](const std::string& on, const std::string& off, const std::string& days)
	{
		RenderPage(*dev, EditorPage("   Pool Light   ", "  New Program   ", on, off, days), /*highlight*/ 0xFF);
	};

	// The Navigator drives to the Program list first; present the list with the equipment already at
	// the highlighted row so SelectEquipment Selects immediately.
	RenderPage(*dev, ProgramListPage("Pool Light      ", 2), /*highlight*/ 2);
	frame();   // ToProgramMenu -> navigator completes on the list (no key)
	frame();   // SelectEquipment: cursor on Pool Light -> Select

	// The empty detail page (Pool Light has no program yet): cursor on Add row (9).
	RenderPage(*dev, EmptyDetailPage("   Pool Light   "), /*highlight*/ 9);
	frame();   // ChooseAction: cursor on Add -> Select -> editor

	// Editor opens with defaults ON 1:00 PM / OFF 1:00 PM / All Days. Each closed-loop step reads the
	// echoed value; render the echo, then deliver the next frame.
	render_editor("ON       1:00 PM", "OFF      1:00 PM", "All Days        ");
	frame();   // EnterEditor detects the editor -> SetOnHour (no key)
	frame();   // SetOnHour: 13 -> LineDown (toward 11)
	render_editor("ON      12:00 PM", "OFF      1:00 PM", "All Days        ");
	frame();   // SetOnHour: 12 -> LineDown
	render_editor("ON      11:00 AM", "OFF      1:00 PM", "All Days        ");
	frame();   // SetOnHour: 11 == target -> Select (advance to ON-minute)
	frame();   // SetOnMinute: 0 == target -> Select (advance to OFF-hour)
	render_editor("ON      11:00 AM", "OFF      1:00 PM", "All Days        ");
	frame();   // SetOffHour: 13 -> LineUp (toward 14)
	render_editor("ON      11:00 AM", "OFF      2:00 PM", "All Days        ");
	frame();   // SetOffHour: 14 == target -> Select (advance to OFF-minute)
	frame();   // SetOffMinute: 0 -> LineUp (toward 1)
	render_editor("ON      11:00 AM", "OFF      2:01 PM", "All Days        ");
	frame();   // SetOffMinute: 1 == target -> Select (advance to days)
	frame();   // SetDays: All Days == target -> Select -> SAVE

	// After the days Select the program saves and the panel returns to the detail page with the new
	// program: Verify re-parses it and completes.
	RenderPage(*dev, DetailPage("   Pool Light   ", "ON      11:00 AM", "OFF      2:01 PM", "All Days        "), 9);
	frame();   // Verify -> matches -> Done

	// Emitted key stream: Select(equipment), Select(Add), then per-field:
	//   ON-hour  : LineDown, LineDown (13->12->11), Select
	//   ON-min   : Select (already 0)
	//   OFF-hour : LineUp (13->14), Select
	//   OFF-min  : LineUp (0->1), Select
	//   days     : Select (already All Days)
	const std::vector<uint8_t> expected{
		SELECT,                         // pick Pool Light on the list
		SELECT,                         // Add Program -> editor
		LINE_DOWN, LINE_DOWN, SELECT,   // ON hour 13->12->11, commit
		SELECT,                         // ON minute (already 0), commit
		LINE_UP, SELECT,                // OFF hour 13->14, commit
		LINE_UP, SELECT,                // OFF minute 0->1, commit
		SELECT,                         // days (All Days), commit -> SAVE
	};
	BOOST_REQUIRE_EQUAL(rec.keys.size(), expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(rec.keys[i]), static_cast<int>(expected[i]));
	}

	// The goal completed -> the panel is idle, a fresh request is accepted.
	BOOST_CHECK(dev->CreateControllerProgram(ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f)) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Delete_SelectsDeleteRow_NoConfirm_AndCompletes)
{
	auto dev = MakeDevice(/*emulated*/ true);
	DriveToNormalOperation(*dev);

	KeyRecorder rec;

	auto target = ProgramSpec("Filter Pump", 11, 0, 14, 0, 0x7f);
	BOOST_REQUIRE(dev->DeleteControllerProgram(target) == Capabilities::ActuationResult::Accepted);

	RenderPage(*dev, ProgramListPage("Filter Pump     ", 2), /*highlight*/ 2);
	dev->DeliverStatusFrameForTest();   // navigate to list
	dev->DeliverStatusFrameForTest();   // SelectEquipment: cursor on Filter Pump -> Select

	// Detail page (existing program), cursor already on the Delete row (10).
	RenderPage(*dev, DetailPage("  Filter Pump   ", "ON      11:00 AM", "OFF      2:00 PM", "All Days        "), /*highlight*/ 10);
	dev->DeliverStatusFrameForTest();   // ChooseAction (delete): cursor on Delete -> Select (no confirm)

	// The program is gone immediately: the detail page now shows "No Programs" -> VerifyGone.
	RenderPage(*dev, EmptyDetailPage("  Filter Pump   "), 9);
	dev->DeliverStatusFrameForTest();

	// Exactly: Select(equipment), Select(Delete row). NO confirmation keystroke.
	const std::vector<uint8_t> expected{ SELECT, SELECT };
	BOOST_REQUIRE_EQUAL(rec.keys.size(), expected.size());
	BOOST_CHECK_EQUAL(static_cast<int>(rec.keys[0]), static_cast<int>(expected[0]));
	BOOST_CHECK_EQUAL(static_cast<int>(rec.keys[1]), static_cast<int>(expected[1]));

	BOOST_CHECK(dev->DeleteControllerProgram(target) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(Edit_ChangeProgram_StepsOnlyChangedField_AndCompletes)
{
	auto dev = MakeDevice(/*emulated*/ true);
	DriveToNormalOperation(*dev);

	KeyRecorder rec;

	// Existing: Pool Light, ON 11:00 AM, OFF 2:01 PM, All Days. Desired: same but OFF -> 3:01 PM
	// (15:01). Only the OFF hour changes; every other field is committed with a bare Select.
	auto existing = ProgramSpec("Pool Light", 11, 0, 14, 1, 0x7f);
	auto desired = ProgramSpec("Pool Light", 11, 0, 15, 1, 0x7f);
	BOOST_REQUIRE(dev->EditControllerProgram(existing, desired) == Capabilities::ActuationResult::Accepted);

	auto frame = [&]() { dev->DeliverStatusFrameForTest(); };
	auto render_editor = [&](const std::string& on, const std::string& off, const std::string& days)
	{
		RenderPage(*dev, EditorPage("   Pool Light   ", " Change Program ", on, off, days), /*highlight*/ 0xFF);
	};

	RenderPage(*dev, ProgramListPage("Pool Light      ", 2), /*highlight*/ 2);
	frame();   // navigate to list (no key)
	frame();   // SelectEquipment -> Select

	// Detail page, cursor on the Change row (11).
	RenderPage(*dev, DetailPage("   Pool Light   ", "ON      11:00 AM", "OFF      2:01 PM", "All Days        "), /*highlight*/ 11);
	frame();   // ChooseAction (edit): cursor on Change -> Select -> editor

	// Editor opens pre-filled with the existing values; only the OFF hour changes.
	render_editor("ON      11:00 AM", "OFF      2:01 PM", "All Days        ");
	frame();   // EnterEditor -> SetOnHour (no key)
	frame();   // SetOnHour: 11 == target -> Select
	frame();   // SetOnMinute: 0 == target -> Select
	frame();   // SetOffHour: 14 -> LineUp (toward 15)
	render_editor("ON      11:00 AM", "OFF      3:01 PM", "All Days        ");
	frame();   // SetOffHour: 15 == target -> Select
	frame();   // SetOffMinute: 1 == target -> Select
	frame();   // SetDays: All Days == target -> Select -> SAVE

	RenderPage(*dev, DetailPage("   Pool Light   ", "ON      11:00 AM", "OFF      3:01 PM", "All Days        "), 9);
	frame();   // Verify

	// Only the OFF hour was stepped (one LineUp); every other field committed with a bare Select.
	const std::vector<uint8_t> expected{
		SELECT,            // pick Pool Light
		SELECT,            // Change Program -> editor
		SELECT,            // ON hour (already 11)
		SELECT,            // ON minute (already 0)
		LINE_UP, SELECT,   // OFF hour 14->15, commit
		SELECT,            // OFF minute (already 1)
		SELECT,            // days (All Days), commit -> SAVE
	};
	BOOST_REQUIRE_EQUAL(rec.keys.size(), expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(rec.keys[i]), static_cast<int>(expected[i]));
	}

	BOOST_CHECK(dev->EditControllerProgram(existing, desired) == Capabilities::ActuationResult::Accepted);
}

//--- day-wheel stepping -----------------------------------------------------

BOOST_AUTO_TEST_CASE(Create_StepsDayWheel_ToWeekdays)
{
	auto dev = MakeDevice(/*emulated*/ true);
	DriveToNormalOperation(*dev);

	KeyRecorder rec;

	// Same times as the default so only the days wheel is stepped: All Days -> Weekdays (LineUp
	// order All Days -> Weekends -> Weekdays, so two LineUps).
	auto program = ProgramSpec("Pool Light", 13, 0, 13, 0, 0x1f /* Weekdays */);
	BOOST_REQUIRE(dev->CreateControllerProgram(program) == Capabilities::ActuationResult::Accepted);

	auto frame = [&]() { dev->DeliverStatusFrameForTest(); };
	auto render_editor = [&](const std::string& days)
	{
		RenderPage(*dev, EditorPage("   Pool Light   ", "  New Program   ", "ON       1:00 PM", "OFF      1:00 PM", days), 0xFF);
	};

	RenderPage(*dev, ProgramListPage("Pool Light      ", 2), 2);
	frame();   // navigate to list (no key)
	frame();   // SelectEquipment -> Select

	RenderPage(*dev, EmptyDetailPage("   Pool Light   "), 9);
	frame();   // Add -> editor

	render_editor("All Days        ");
	frame();   // EnterEditor -> SetOnHour (no key)
	frame();   // SetOnHour: 13 == target -> Select
	frame();   // SetOnMinute: 0 == target -> Select
	frame();   // SetOffHour: 13 == target -> Select
	frame();   // SetOffMinute: 0 == target -> Select
	frame();   // SetDays: All Days -> LineUp (toward Weekdays)
	render_editor("Weekends        ");
	frame();   // SetDays: Weekends -> LineUp
	render_editor("Weekdays        ");
	frame();   // SetDays: Weekdays == target -> Select -> SAVE

	RenderPage(*dev, DetailPage("   Pool Light   ", "ON       1:00 PM", "OFF      1:00 PM", "Weekdays        "), 9);
	frame();   // Verify

	const std::vector<uint8_t> expected{
		SELECT,             // pick Pool Light
		SELECT,             // Add -> editor
		SELECT,             // ON hour (13)
		SELECT,             // ON minute (0)
		SELECT,             // OFF hour (13)
		SELECT,             // OFF minute (0)
		LINE_UP, LINE_UP, SELECT,   // days All Days -> Weekends -> Weekdays, commit -> SAVE
	};
	BOOST_REQUIRE_EQUAL(rec.keys.size(), expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(rec.keys[i]), static_cast<int>(expected[i]));
	}
}

BOOST_AUTO_TEST_SUITE_END()
