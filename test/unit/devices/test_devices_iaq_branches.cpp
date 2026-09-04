#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq/iaq_aquapure_writer.h"
#include "jandy/devices/iaq/iaq_command_sink.h"
#include "jandy/devices/iaq/iaq_page_model.h"
#include "jandy/devices/iaq/iaq_schedule_writer.h"
#include "jandy/devices/iaq/iaq_spaswitch_writer.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/iaq/iaq_message_page_button.h"

#include "scheduling/controller_schedule.h"
#include "utility/screen_data_page.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// The IAQ (AqualinkTouch 0x33) WRITE state machines, driven directly against a
// hand-built PageModel and a recording command sink -- no bus, no harness.
//
// The existing suites cover each writer's happy path; these cases pin the
// page-GATE and fail-safe arms that make the writers safe on a shared panel UI:
// a hop that lands on the wrong page must DWELL or re-navigate, never press on.
// They also pin the bounded searches (picker scroll, poll backstop) and the
// value encodings (day touch cells, the 12-hour submit string).
//=============================================================================

namespace
{
	// --- IAQ page ids (mirrors the writers' private constants) ---------------
	constexpr uint8_t PAGE_HOME{ 0x01 };
	constexpr uint8_t PAGE_MENU{ 0x0f };
	constexpr uint8_t PAGE_SETUP{ 0x14 };
	constexpr uint8_t PAGE_SET_SWG{ 0x30 };
	constexpr uint8_t PAGE_SCHEDULE_LIST{ 0x28 };
	constexpr uint8_t PAGE_TIME_PICKER{ 0x29 };
	constexpr uint8_t PAGE_DEVICE_PICKER{ 0x38 };
	constexpr uint8_t PAGE_SPA_REMOTES{ 0x3a };
	constexpr uint8_t PAGE_SPA_SWITCH_DETAIL{ 0x3b };
	constexpr uint8_t PAGE_UNKNOWN{ 0x77 };

	// --- commands ------------------------------------------------------------
	constexpr uint8_t CMD_DWELL{ 0x00 };
	constexpr uint8_t CMD_MENU_OR_BACK{ 0x02 };
	constexpr uint8_t CMD_ADD_PROGRAM{ 0x11 };       // also: menu pos0 -> Schedule list; AM/PM toggle
	constexpr uint8_t CMD_PICKER_SCROLL{ 0x12 };
	constexpr uint8_t CMD_PICKER_OK{ 0x13 };
	constexpr uint8_t CMD_DELETE_PROGRAM{ 0x13 };
	constexpr uint8_t CMD_CONFIRM_OK{ 0x01 };
	constexpr uint8_t CMD_EDIT_PROGRAM{ 0x12 };
	constexpr uint8_t CMD_AMPM_TOGGLE{ 0x11 };       // on the time picker, 0x11 flips AM <-> PM
	constexpr uint8_t CMD_OPEN_ON_FIELD{ 0x21 };
	constexpr uint8_t CMD_OPEN_OFF_FIELD{ 0x22 };
	constexpr uint8_t CMD_SUBMIT_VALUE{ 0x80 };
	constexpr uint8_t CMD_MENU_TO_SETUP{ 0x15 };
	constexpr uint8_t CMD_SPASWITCH_SCROLL{ 0x15 };
	constexpr uint8_t CMD_OPEN_SPASWITCH_DETAIL{ 0x16 };

	// Day touch cells on the schedule list.
	constexpr uint8_t CMD_DAY_ALL{ 0x17 };
	constexpr uint8_t CMD_DAY_MON{ 0x18 };
	constexpr uint8_t CMD_DAY_WKDAYS{ 0x1f };
	constexpr uint8_t CMD_DAY_WKENDS{ 0x20 };

	// Day-of-week bitmask (bit0 = Monday .. bit6 = Sunday).
	constexpr uint8_t DAYS_ALL{ 0x7f };
	constexpr uint8_t DAYS_WEEKDAYS{ 0x1f };
	constexpr uint8_t DAYS_WEEKENDS{ 0x60 };
	constexpr uint8_t DAYS_WEDNESDAY{ 0x04 };

	// Records what a writer pushes at the poll-ACK channel. Dwells (0x00) are counted but kept out
	// of `issued` so a test can assert the meaningful keystrokes without counting settle polls.
	class RecordingSink : public IAQ::ICommandSink
	{
	public:
		void IssueCommand(uint8_t command) override
		{
			last = command;
			if (CMD_DWELL == command) { ++dwells; } else { issued.push_back(command); }
		}
		void ArmControlValue(std::string value) override { control_values.push_back(std::move(value)); }
		bool IsBusy() const override { return false; }

		uint8_t last{ CMD_DWELL };
		std::size_t dwells{ 0 };
		std::vector<uint8_t> issued;
		std::vector<std::string> control_values;
	};

	IAQ::PageModel MakePage(uint8_t page_id)
	{
		IAQ::PageModel page;
		page.OnPageStart(page_id);
		return page;
	}

	Scheduling::ControllerSchedule MakeProgram(const std::string& target, uint8_t days,
		int on_hour, int on_minute, int off_hour, int off_minute)
	{
		Scheduling::ControllerSchedule program;
		program.target = target;
		program.days_of_week = days;
		program.on_hour = on_hour;
		program.on_minute = on_minute;
		program.off_hour = off_hour;
		program.off_minute = off_minute;
		return program;
	}

	//-------------------------------------------------------------------------
	// ScheduleWriter harness
	//-------------------------------------------------------------------------

	struct ScheduleRig
	{
		ScheduleRig() : device_id(JandyDeviceId(0x33)), status(18) {}

		JandyDeviceType device_id;
		IAQ::ScheduleWriter writer;
		RecordingSink sink;
		Utility::ScreenDataPage status;
		IAQ::PageModel page{ MakePage(PAGE_HOME) };

		void Step(int polls)
		{
			for (int i = 0; i < polls; ++i) { writer.ProcessStep(page, status, sink, device_id); }
		}

		// Pump until one more non-dwell command is emitted; returns it (0x00 when none appeared).
		uint8_t PumpForCommand(int max_polls = 12)
		{
			const std::size_t before = sink.issued.size();
			for (int i = 0; (i < max_polls) && (sink.issued.size() == before); ++i)
			{
				writer.ProcessStep(page, status, sink, device_id);
			}
			return (sink.issued.size() > before) ? sink.issued.back() : CMD_DWELL;
		}

		void ShowPage(uint8_t page_id) { page.OnPageStart(page_id); }
	};

	// Drive an armed CREATE goal from the schedule list all the way to the day press, feeding the
	// writer the page the master would render at each hop. Leaves the goal in its Verify phase.
	void DriveCreateToDayPress(ScheduleRig& rig, const Scheduling::ControllerSchedule& program)
	{
		rig.ShowPage(PAGE_SCHEDULE_LIST);                       // navigate -> Add Program
		rig.Step(8);
		rig.ShowPage(PAGE_DEVICE_PICKER);                       // pick the target device, then confirm
		rig.page.SetDevicePickerRow(0, "Devices");
		rig.page.SetDevicePickerRow(1, program.target);
		rig.Step(14);
		rig.ShowPage(PAGE_SCHEDULE_LIST);                       // open the ON field
		rig.Step(8);
		rig.ShowPage(PAGE_TIME_PICKER);                         // submit the ON time
		rig.status[2].Text = (program.on_hour >= 12) ? "PM" : "AM";
		rig.Step(8);
		rig.ShowPage(PAGE_SCHEDULE_LIST);                       // open the OFF field
		rig.Step(8);
		rig.ShowPage(PAGE_TIME_PICKER);                         // submit the OFF time
		rig.status[2].Text = (program.off_hour >= 12) ? "PM" : "AM";
		rig.Step(8);
		rig.ShowPage(PAGE_SCHEDULE_LIST);                       // press the day cell
		rig.Step(8);
	}

	//-------------------------------------------------------------------------
	// SpaSwitchWriter harness
	//-------------------------------------------------------------------------

	struct SpaSwitchRig
	{
		SpaSwitchRig() : device_id(JandyDeviceId(0x33)) {}

		JandyDeviceType device_id;
		IAQ::SpaSwitchWriter writer;
		RecordingSink sink;
		IAQ::PageModel page{ MakePage(PAGE_HOME) };

		void Step(int polls)
		{
			for (int i = 0; i < polls; ++i) { writer.ProcessStep(page, sink, nullptr, device_id); }
		}

		uint8_t PumpForCommand(int max_polls = 12)
		{
			const std::size_t before = sink.issued.size();
			for (int i = 0; (i < max_polls) && (sink.issued.size() == before); ++i)
			{
				writer.ProcessStep(page, sink, nullptr, device_id);
			}
			return (sink.issued.size() > before) ? sink.issued.back() : CMD_DWELL;
		}

		void ShowPage(uint8_t page_id) { page.OnPageStart(page_id); }
	};

	//-------------------------------------------------------------------------
	// AquaPureWriter harness
	//-------------------------------------------------------------------------

	struct AquaPureRig
	{
		AquaPureRig() : device_id(JandyDeviceId(0x33)) {}

		JandyDeviceType device_id;
		IAQ::AquaPureWriter writer;
		RecordingSink sink;
		IAQ::PageModel page{ MakePage(PAGE_HOME) };

		void Step(int polls)
		{
			for (int i = 0; i < polls; ++i) { writer.ProcessStep(page, sink, device_id); }
		}

		uint8_t PumpForCommand(int max_polls = 12)
		{
			const std::size_t before = sink.issued.size();
			for (int i = 0; (i < max_polls) && (sink.issued.size() == before); ++i)
			{
				writer.ProcessStep(page, sink, device_id);
			}
			return (sink.issued.size() > before) ? sink.issued.back() : CMD_DWELL;
		}

		void ShowPage(uint8_t page_id) { page.OnPageStart(page_id); }
	};
}
// unnamed namespace

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQ_ScheduleWriterBranches_TestSuite)
//=============================================================================

//-----------------------------------------------------------------------------
// Arming
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DeleteAndEdit_AreRejectedWhileTheChannelIsBusy)
{
	// Busy is TRANSIENT (retry shortly) and distinct from NotSupported (no capable controller):
	// another writer or a draining command sequence owns the shared panel UI right now.
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);

	BOOST_CHECK(rig.writer.QueueDelete(program, /*emulated=*/true, /*channel_busy=*/true, rig.device_id)
		== Capabilities::ActuationResult::Busy);
	BOOST_CHECK(rig.writer.QueueEdit(program, program, /*emulated=*/true, /*channel_busy=*/true, rig.device_id)
		== Capabilities::ActuationResult::Busy);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(DeleteAndEdit_AreRejectedWhileAnotherGoalIsInFlight)
{
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);

	BOOST_REQUIRE(rig.writer.QueueCreate(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(rig.writer.QueueDelete(program, true, false, rig.device_id) == Capabilities::ActuationResult::Busy);
	BOOST_CHECK(rig.writer.QueueEdit(program, program, true, false, rig.device_id) == Capabilities::ActuationResult::Busy);
}

BOOST_AUTO_TEST_CASE(DeleteAndEdit_WithoutATarget_AreInvalid)
{
	ScheduleRig rig;
	const auto no_target = MakeProgram("", DAYS_ALL, 9, 0, 17, 0);
	const auto valid = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);

	BOOST_CHECK(rig.writer.QueueDelete(no_target, true, false, rig.device_id) == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK(rig.writer.QueueEdit(no_target, valid, true, false, rig.device_id) == Capabilities::ActuationResult::InvalidValue);
	// The DESIRED side of an edit is gated by the same feasibility rule as create.
	auto arbitrary_days = valid;
	arbitrary_days.days_of_week = 0x15;   // Mon+Wed+Fri -- not a selection the controller can pick
	BOOST_CHECK(rig.writer.QueueEdit(valid, arbitrary_days, true, false, rig.device_id) == Capabilities::ActuationResult::InvalidValue);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(APassiveDeviceCannotDeleteOrEdit)
{
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);

	BOOST_CHECK(rig.writer.QueueDelete(program, /*emulated=*/false, false, rig.device_id) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(rig.writer.QueueEdit(program, program, /*emulated=*/false, false, rig.device_id) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(WithNoGoalArmed_ProcessStepIsANoOp)
{
	ScheduleRig rig;
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(5);

	BOOST_CHECK(rig.sink.issued.empty());
	BOOST_CHECK_EQUAL(rig.sink.dwells, 0u);
}

//-----------------------------------------------------------------------------
// Page-gated navigation to the Schedule list
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(NavigateToList_UnwindsFromHomeAndOpensFromTheMenu)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// Home: press Menu/Back to unwind toward the menu.
	rig.ShowPage(PAGE_HOME);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));

	// An unrecognised sub-page takes the same default arm rather than pressing a page button.
	rig.ShowPage(PAGE_UNKNOWN);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));

	// The menu: press the Schedule entry.
	rig.ShowPage(PAGE_MENU);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_ADD_PROGRAM));
}

BOOST_AUTO_TEST_CASE(AddProgram_OffTheList_ReNavigatesInsteadOfPressing)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// Reach the list so the phase advances to AddProgram, then have the master render something
	// else: the writer must fall back to navigation, not fire Add Program at the wrong page.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(1);                       // NavigateToList -> AddProgram (no keypress)
	rig.ShowPage(PAGE_SETUP);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));
}

//-----------------------------------------------------------------------------
// The device picker
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DevicePicker_OffPage_DwellsRatherThanClickingARow)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);                       // navigate -> Add Program (+ settle)
	BOOST_REQUIRE_EQUAL(rig.sink.issued.size(), 1u);

	// The picker never renders: the writer dwells indefinitely instead of clicking blind.
	rig.ShowPage(PAGE_HOME);
	rig.Step(10);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.sink.last), static_cast<int>(CMD_DWELL));
	BOOST_CHECK(rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(DevicePicker_TargetNeverFound_ScrollsThenAbandonsTheGoal)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Waterfall", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);

	// A picker that never shows "Waterfall": the search is bounded (12 scrolls) and the goal is
	// abandoned rather than scrolling the list forever.
	rig.ShowPage(PAGE_DEVICE_PICKER);
	rig.page.SetDevicePickerRow(1, "Filter Pump");
	rig.page.SetDevicePickerRow(2, "Pool Light");
	for (int i = 0; (i < 200) && rig.writer.HasPendingGoal(); ++i) { rig.Step(1); }

	BOOST_CHECK(!rig.writer.HasPendingGoal());
	// Every command after the Add Program press was a picker scroll -- no row was ever clicked.
	BOOST_REQUIRE_GE(rig.sink.issued.size(), 2u);
	for (std::size_t i = 1; i < rig.sink.issued.size(); ++i)
	{
		BOOST_CHECK_EQUAL(static_cast<int>(rig.sink.issued[i]), static_cast<int>(CMD_PICKER_SCROLL));
	}
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 1u + 12u);   // Add Program + IAQ_SCHEDULE_MAX_SCROLLS
}

//-----------------------------------------------------------------------------
// The time picker
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(TimeField_OffTheListAndOffThePicker_Dwells)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);
	rig.ShowPage(PAGE_DEVICE_PICKER);
	rig.page.SetDevicePickerRow(1, "Pool Light");
	rig.Step(14);                       // click the row + confirm -> SetOnTime
	const auto after_pick = rig.sink.issued.size();

	// Still on the picker: the ON field cannot be opened from here, so dwell.
	rig.Step(4);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), after_pick);

	// The list renders -> open the ON field (0x21).
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_OPEN_ON_FIELD));

	// The time picker does not come up: dwell rather than submitting into whatever is on screen.
	rig.ShowPage(PAGE_HOME);
	rig.Step(10);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), after_pick + 1u);
	BOOST_CHECK(rig.sink.control_values.empty());
}

BOOST_AUTO_TEST_CASE(TimeField_WithoutAMeridiemLine_WaitsForThePickerToFinishRendering)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueCreate(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);
	rig.ShowPage(PAGE_DEVICE_PICKER);
	rig.page.SetDevicePickerRow(1, "Pool Light");
	rig.Step(14);
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);                        // open the ON field
	const auto before = rig.sink.issued.size();

	// Line 2 is still blank (the picker is half-rendered): neither toggle nor submit.
	rig.ShowPage(PAGE_TIME_PICKER);
	rig.status[2].Text = "";
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), before);
	BOOST_CHECK(rig.sink.control_values.empty());

	// Garbage on the meridiem line is treated the same way.
	rig.status[2].Text = "--";
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), before);

	// It finally renders as PM while 09:00 is AM -> flip the meridiem first.
	rig.status[2].Text = "PM";
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_AMPM_TOGGLE));
	BOOST_CHECK(rig.sink.control_values.empty());
}

//-----------------------------------------------------------------------------
// Create, end to end
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Create_EndToEnd_EmitsTheDecodedKeySequenceAndCompletesOnVerify)
{
	ScheduleRig rig;
	// Midnight exercises the 12-hour wrap (0 -> 12) in the submit value.
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 0, 30, 13, 45);
	BOOST_REQUIRE(rig.writer.QueueCreate(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	DriveCreateToDayPress(rig, program);

	const std::vector<uint8_t> expected{
		CMD_ADD_PROGRAM,                                   // list pos0 -> Add Program
		static_cast<uint8_t>(CMD_PICKER_OK + 1),           // click visible picker row 1
		CMD_PICKER_OK,                                     // confirm the highlighted device
		CMD_OPEN_ON_FIELD, CMD_SUBMIT_VALUE,
		CMD_OPEN_OFF_FIELD, CMD_SUBMIT_VALUE,
		CMD_DAY_ALL
	};
	BOOST_CHECK_EQUAL_COLLECTIONS(rig.sink.issued.begin(), rig.sink.issued.end(), expected.begin(), expected.end());

	// Times ride the control-data handshake as "<field>HH:MM" on a 12-hour clock.
	BOOST_REQUIRE_EQUAL(rig.sink.control_values.size(), 2u);
	BOOST_CHECK_EQUAL(rig.sink.control_values[0], "112:30");   // 00:30 -> 12:30 AM
	BOOST_CHECK_EQUAL(rig.sink.control_values[1], "101:45");   // 13:45 -> 1:45 PM

	// Verify: the list does not yet carry the program, so the writer dwells and holds the goal.
	rig.Step(6);
	BOOST_CHECK(rig.writer.HasPendingGoal());
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), expected.size());

	// The master re-renders the list with the new program -> the goal completes.
	rig.page.SetScheduleRow(1, "Pool Light\t12:30 AM\t1:45 PM\tAll");
	rig.Step(2);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(SetDay_OffTheList_DwellsUntilTheListReRenders)
{
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);
	BOOST_REQUIRE(rig.writer.QueueCreate(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);
	rig.ShowPage(PAGE_DEVICE_PICKER);
	rig.page.SetDevicePickerRow(1, "Pool Light");
	rig.Step(14);
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);
	rig.ShowPage(PAGE_TIME_PICKER);
	rig.status[2].Text = "AM";
	rig.Step(8);
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(8);
	rig.ShowPage(PAGE_TIME_PICKER);
	rig.status[2].Text = "PM";
	rig.Step(8);
	const auto before = rig.sink.issued.size();

	// SetDay with the picker still up: no day cell is pressed.
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), before);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_DAY_ALL));
}

BOOST_AUTO_TEST_CASE(DaySelection_MapsToTheControllersTouchCells)
{
	// The controller can only express all / weekdays / weekends / one day; each maps to its own
	// touch cell, with single days running consecutively from Monday.
	const std::vector<std::pair<uint8_t, uint8_t>> cases{
		{ DAYS_ALL,       CMD_DAY_ALL },
		{ DAYS_WEEKDAYS,  CMD_DAY_WKDAYS },
		{ DAYS_WEEKENDS,  CMD_DAY_WKENDS },
		{ DAYS_WEDNESDAY, static_cast<uint8_t>(CMD_DAY_MON + 2) }
	};

	for (const auto& [days, expected_command] : cases)
	{
		ScheduleRig rig;
		const auto program = MakeProgram("Pool Light", days, 9, 0, 17, 0);
		BOOST_REQUIRE(rig.writer.QueueCreate(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

		DriveCreateToDayPress(rig, program);

		BOOST_REQUIRE(!rig.sink.issued.empty());
		BOOST_CHECK_EQUAL(static_cast<int>(rig.sink.issued.back()), static_cast<int>(expected_command));
	}
}

//-----------------------------------------------------------------------------
// Delete
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Delete_EndToEnd_ClicksTheRowConfirmsAndVerifiesItIsGone)
{
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);
	BOOST_REQUIRE(rig.writer.QueueDelete(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	// The list is up but has not populated yet: wait, do not click row 0.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(1);                        // NavigateToList -> SelectRow
	rig.Step(5);
	BOOST_CHECK(rig.sink.issued.empty());

	// Rows arrive; ordinal 2 is the match -> click it (0x22 + ordinal).
	rig.page.SetScheduleRow(1, "Filter Pump\t11:00 AM\t2:00 PM\tAll");
	rig.page.SetScheduleRow(2, "Pool Light\t9:00 AM\t5:00 PM\tAll");
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x24);

	// The highlighted row is deleted, then confirmed.
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_DELETE_PROGRAM));
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_CONFIRM_OK));

	// VerifyGone: while the row is still listed the writer dwells and keeps the goal.
	rig.Step(6);
	BOOST_CHECK(rig.writer.HasPendingGoal());

	// The master re-renders without it -> done.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.page.SetScheduleRow(1, "Filter Pump\t11:00 AM\t2:00 PM\tAll");
	rig.Step(3);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(Delete_WithNoMatchingRow_AbandonsWithoutPressingDelete)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueDelete(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.page.SetScheduleRow(1, "Filter Pump\t11:00 AM\t2:00 PM\tAll");
	rig.page.SetScheduleRow(2, "not a schedule row at all");
	rig.Step(6);

	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK(rig.sink.issued.empty());   // nothing was pressed on a list we could not match
}

BOOST_AUTO_TEST_CASE(Delete_OffTheList_DwellsAtEveryStage)
{
	ScheduleRig rig;
	const auto program = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);
	BOOST_REQUIRE(rig.writer.QueueDelete(program, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	// SelectRow off the list -> dwell.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(1);
	rig.ShowPage(PAGE_HOME);
	rig.Step(5);
	BOOST_CHECK(rig.sink.issued.empty());

	// Back on the list with the matching row -> click it, advancing to PressDelete.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.page.SetScheduleRow(1, "Pool Light\t9:00 AM\t5:00 PM\tAll");
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x23);

	// PressDelete off the list -> dwell rather than firing Delete at another page.
	rig.ShowPage(PAGE_SETUP);
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 1u);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.page.SetScheduleRow(1, "Pool Light\t9:00 AM\t5:00 PM\tAll");
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_DELETE_PROGRAM));
}

//-----------------------------------------------------------------------------
// Edit
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Edit_ClicksTheExistingRowThenPressesEdit)
{
	ScheduleRig rig;
	const auto existing = MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0);
	const auto desired = MakeProgram("Pool Light", DAYS_WEEKENDS, 10, 0, 16, 30);
	BOOST_REQUIRE(rig.writer.QueueEdit(existing, desired, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.page.SetScheduleRow(1, "Pool Light\t9:00 AM\t5:00 PM\tAll");
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x23);   // row 1 -> 0x22 + 1

	// PressEdit is page-gated too.
	rig.ShowPage(PAGE_HOME);
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 1u);

	rig.ShowPage(PAGE_SCHEDULE_LIST);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_EDIT_PROGRAM));

	// Edit then re-sets the fields with the create flow's keys, from the DESIRED program.
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_OPEN_ON_FIELD));
	rig.ShowPage(PAGE_TIME_PICKER);
	rig.status[2].Text = "AM";
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_SUBMIT_VALUE));
	BOOST_REQUIRE_EQUAL(rig.sink.control_values.size(), 1u);
	BOOST_CHECK_EQUAL(rig.sink.control_values[0], "110:00");
}

//-----------------------------------------------------------------------------
// The overall backstop
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(AGoalThatNeverProgresses_IsAbandonedByThePollBackstop)
{
	ScheduleRig rig;
	BOOST_REQUIRE(rig.writer.QueueDelete(MakeProgram("Pool Light", DAYS_ALL, 9, 0, 17, 0), true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// A master that never renders the schedule list: the writer dwells, but not forever.
	rig.ShowPage(PAGE_SCHEDULE_LIST);
	rig.Step(1);                        // -> SelectRow
	rig.ShowPage(PAGE_HOME);
	rig.Step(399);
	BOOST_CHECK(rig.writer.HasPendingGoal());
	rig.Step(2);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK(rig.sink.issued.empty());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQ_SpaSwitchWriterBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(Navigate_WalksHomeToMenuToSetupToSpaRemotesToTheDetail)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 2, "Pool Heat", /*emulation_active=*/true, /*channel_busy=*/false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// Home -> Menu/Back.
	rig.ShowPage(PAGE_HOME);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));

	// Menu -> Setup (0x15).
	rig.ShowPage(PAGE_MENU);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_TO_SETUP));

	// Setup with the button table not yet rendered: dwell rather than pressing a guessed index.
	rig.ShowPage(PAGE_SETUP);
	const auto before_setup = rig.sink.issued.size();
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), before_setup);

	// The button table arrives -> press the labelled entry (0x11 + index).
	rig.page.UpsertButton(0, "System", Messages::ButtonStatuses::Off);
	rig.page.UpsertButton(3, "Spa Remotes", Messages::ButtonStatuses::Off);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x14);

	// Spa Remotes -> open the 4-Function detail (0x16).
	rig.ShowPage(PAGE_SPA_REMOTES);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_OPEN_SPASWITCH_DETAIL));

	// The detail: row-select 1:2 (ordinal 2 -> 0x15 + 2).
	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x17);
}

BOOST_AUTO_TEST_CASE(Navigate_AnUnknownSubPage_UnwindsRatherThanPressing)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 1, "Spa Jets", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_UNKNOWN);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));
}

BOOST_AUTO_TEST_CASE(LosingTheDetailPage_ReNavigatesInsteadOfCommitting)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 2, "Pool Heat", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	rig.Step(1);                        // Navigate -> SelectRow (no keypress)

	// The master navigates away before the row-select: fall back to navigation.
	rig.ShowPage(PAGE_HOME);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));
	BOOST_CHECK(rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(LosingTheDetailPage_DuringTheFunctionSearch_ReNavigates)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 2, "Pool Heat", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x17);   // row-select -> FindFunction

	rig.ShowPage(PAGE_SETUP);
	// Setup is a navigation waypoint, so the re-navigation presses the Spa Remotes button once
	// the table renders; until then it dwells.
	const auto before = rig.sink.issued.size();
	rig.Step(6);
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), before);
	BOOST_CHECK(rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(PickerThatWrapsBackToItsFirstRow_AbandonsTheGoal)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 2, "Waterfall", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	rig.page.SetSpaSwitchPickerRow(1, "Filter Pump");
	rig.page.SetSpaSwitchPickerRow(2, "Spa");
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x17);   // row-select

	// First look at the picker: "Waterfall" is absent -> scroll.
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_SPASWITCH_SCROLL));

	// The scroll wrapped: the first row repeats, so the whole list has been cycled without a hit.
	rig.Step(8);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 2u);   // row-select + exactly one scroll
}

BOOST_AUTO_TEST_CASE(PickerThatKeepsChanging_ScrollsAtMostTenTimes)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 1, "Waterfall", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	rig.page.SetSpaSwitchPickerRow(1, "Page 0");
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x16);   // row-select 1:1 (ordinal 1)

	// Each scroll produces a genuinely new page, so wrap-detection never fires and only the hard
	// scroll bound stops the search.
	for (int page_index = 1; (page_index < 40) && rig.writer.HasPendingGoal(); ++page_index)
	{
		rig.page.ClearSpaSwitchPickerRows();
		rig.page.SetSpaSwitchPickerRow(1, "Page " + std::to_string(page_index));
		rig.Step(8);
	}

	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 1u + 10u);   // row-select + IAQ_SPASWITCH_MAX_SCROLLS
}

BOOST_AUTO_TEST_CASE(WithoutADataHub_VerifyDwellsUntilThePollBackstopAbandonsTheGoal)
{
	SpaSwitchRig rig;
	BOOST_REQUIRE(rig.writer.Queue(1, 2, "Pool Heat", true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	rig.page.SetSpaSwitchPickerRow(3, "Pool Heat");
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x17);                     // row-select
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x1f);                     // commit slot 3

	// Verify has no DataHub to confirm against: it dwells, bounded by the poll backstop.
	for (int i = 0; (i < 500) && rig.writer.HasPendingGoal(); ++i) { rig.Step(1); }
	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK_EQUAL(rig.sink.issued.size(), 2u);
}

BOOST_AUTO_TEST_CASE(WithNoGoalArmed_ProcessStepIsANoOp)
{
	SpaSwitchRig rig;
	rig.ShowPage(PAGE_SPA_SWITCH_DETAIL);
	rig.Step(5);

	BOOST_CHECK(rig.sink.issued.empty());
	BOOST_CHECK_EQUAL(rig.sink.dwells, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_AUTO_TEST_SUITE(IAQ_AquaPureWriterBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(AnAquaPurePageWithNoButtonsYet_DwellsInsteadOfPressing)
{
	AquaPureRig rig;
	BOOST_REQUIRE(rig.writer.QueuePercentage(60, Kernel::BodyOfWaterIds::Pool, true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// Arrived on the AquaPure page, but the master has not pushed the row buttons yet.
	rig.ShowPage(PAGE_SET_SWG);
	rig.Step(6);
	BOOST_CHECK(rig.sink.issued.empty());
	BOOST_CHECK_EQUAL(static_cast<int>(rig.sink.last), static_cast<int>(CMD_DWELL));

	// The rows arrive -> the Pool row is pressed by label.
	rig.page.UpsertButton(0, "Pool 30%", Messages::ButtonStatuses::Off);
	rig.page.UpsertButton(1, "Spa 30%", Messages::ButtonStatuses::Off);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x11);
}

BOOST_AUTO_TEST_CASE(LosingTheAquaPurePageBeforeSubmitting_StartsOverRatherThanFiringTheValue)
{
	AquaPureRig rig;
	BOOST_REQUIRE(rig.writer.QueuePercentage(60, Kernel::BodyOfWaterIds::Pool, true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SET_SWG);
	rig.page.UpsertButton(0, "Pool 30%", Messages::ButtonStatuses::Off);
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x11);   // field selected

	// The master navigates home before the submit: no value may be fired into that page.
	rig.ShowPage(PAGE_HOME);
	rig.Step(6);
	BOOST_CHECK(rig.sink.control_values.empty());

	// Back on the AquaPure page the walk resumes from the field selection.
	rig.ShowPage(PAGE_SET_SWG);
	rig.page.UpsertButton(0, "Pool 30%", Messages::ButtonStatuses::Off);
	for (int i = 0; (i < 40) && rig.sink.control_values.empty(); ++i) { rig.Step(1); }
	BOOST_REQUIRE_EQUAL(rig.sink.control_values.size(), 1u);
	BOOST_CHECK_EQUAL(rig.sink.control_values[0], "160");
}

BOOST_AUTO_TEST_CASE(AfterSubmitting_TheWriterUnwindsToHomeAndFinishes)
{
	AquaPureRig rig;
	BOOST_REQUIRE(rig.writer.QueuePercentage(75, Kernel::BodyOfWaterIds::Spa, true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SET_SWG);
	rig.page.UpsertButton(0, "Pool 30%", Messages::ButtonStatuses::Off);
	rig.page.UpsertButton(1, "Spa 30%", Messages::ButtonStatuses::Off);
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x12);                     // Spa row
	BOOST_REQUIRE_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_SUBMIT_VALUE));
	BOOST_REQUIRE_EQUAL(rig.sink.control_values.size(), 1u);
	BOOST_CHECK_EQUAL(rig.sink.control_values[0], "175");

	// ReturnHome: still on the settings page -> press Menu/Back.
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));

	// Home renders: the goal completes and the panel is left where the actuators expect it.
	rig.ShowPage(PAGE_HOME);
	rig.Step(8);
	BOOST_CHECK(!rig.writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(AGoalThatNeverProgresses_IsAbandonedByThePollBackstop)
{
	AquaPureRig rig;
	BOOST_REQUIRE(rig.writer.QueuePercentage(60, Kernel::BodyOfWaterIds::Pool, true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);

	// Sitting on the AquaPure page whose rows never render: dwell, but not forever.
	rig.ShowPage(PAGE_SET_SWG);
	for (int i = 0; (i < 400) && rig.writer.HasPendingGoal(); ++i) { rig.Step(1); }

	BOOST_CHECK(!rig.writer.HasPendingGoal());
	BOOST_CHECK(rig.sink.issued.empty());
}

BOOST_AUTO_TEST_CASE(ObserveMenuPage_ForgetsAStaleRouteWhenTheMenuNoLongerListsAquaPure)
{
	AquaPureRig rig;

	// A menu that advertises the entry teaches the route...
	rig.ShowPage(PAGE_MENU);
	rig.page.UpsertButton(2, "AquaPure", Messages::ButtonStatuses::Off);
	rig.writer.ObserveMenuPage(rig.page);
	BOOST_REQUIRE(rig.writer.QueuePercentage(50, Kernel::BodyOfWaterIds::Pool, true, false, rig.device_id)
		== Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x13);   // 0x11 + learned index 2

	// ...and a later menu WITHOUT it drops the learned index, so the next walk falls back to the
	// confirmed blind key rather than pressing a button that has moved.
	AquaPureRig fresh;
	fresh.ShowPage(PAGE_MENU);
	fresh.page.UpsertButton(2, "AquaPure", Messages::ButtonStatuses::Off);
	fresh.writer.ObserveMenuPage(fresh.page);
	fresh.page.EraseButton(2);
	fresh.page.UpsertButton(2, "Set Temperature", Messages::ButtonStatuses::Off);
	fresh.writer.ObserveMenuPage(fresh.page);

	BOOST_REQUIRE(fresh.writer.QueuePercentage(50, Kernel::BodyOfWaterIds::Pool, true, false, fresh.device_id)
		== Capabilities::ActuationResult::Accepted);
	BOOST_CHECK_EQUAL(static_cast<int>(fresh.PumpForCommand()), 0x19);   // the confirmed menu -> AquaPure key
}

BOOST_AUTO_TEST_CASE(BoostOff_PressesTheBoostRowAndSubmitsNoValue)
{
	AquaPureRig rig;
	BOOST_REQUIRE(rig.writer.QueueBoost(/*enable=*/false, true, false, rig.device_id) == Capabilities::ActuationResult::Accepted);

	rig.ShowPage(PAGE_SET_SWG);
	// A too-short button name can never contain the needle -- it must not be pressed by mistake.
	rig.page.UpsertButton(0, "Po", Messages::ButtonStatuses::Off);
	rig.page.UpsertButton(1, "Pool 30%", Messages::ButtonStatuses::Off);
	rig.page.UpsertButton(2, "Quick Boost", Messages::ButtonStatuses::Off);

	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), 0x13);   // 0x11 + index 2 (Quick Boost)

	// A boost press IS the change: no absolute value follows it, and the writer unwinds home.
	BOOST_CHECK(rig.sink.control_values.empty());
	BOOST_CHECK_EQUAL(static_cast<int>(rig.PumpForCommand()), static_cast<int>(CMD_MENU_OR_BACK));
	BOOST_CHECK(rig.sink.control_values.empty());
}

BOOST_AUTO_TEST_CASE(WithNoGoalArmed_ProcessStepIsANoOp)
{
	AquaPureRig rig;
	rig.ShowPage(PAGE_SET_SWG);
	rig.Step(5);

	BOOST_CHECK(rig.sink.issued.empty());
	BOOST_CHECK_EQUAL(rig.sink.dwells, 0u);
}

BOOST_AUTO_TEST_SUITE_END()
