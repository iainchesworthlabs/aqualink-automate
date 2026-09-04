#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/test/unit_test.hpp>

#include "devices/onetouch/onetouch_goals.h"
#include "devices/onetouch/onetouch_keypad.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "navigation/menu_model.h"
#include "navigation/navigator.h"
#include "navigation/onetouch_menu_model.h"
#include "scheduling/controller_schedule.h"
#include "utility/screen_data_page.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Devices::OneTouch;

//=============================================================================
// OneTouch on-demand keypad goals (ToggleGoal / ValueEditGoal / BoostGoal /
// SpaSwitchGoal / ScheduleWriteGoal) driven DIRECTLY through a KeypadContext
// against the real OneTouch MenuModel + Navigator, with a hand-built screen.
//
// Each Step() is one Status cycle: the test presents the screen the controller
// would be showing, positions the cursor, and asserts the goal's status plus the
// single key it emits. No device, no bus - the goals are pure phase machines
// over the shared keypad.
//=============================================================================

namespace
{
	using Navigation::NavKeyCommand;
	using Navigation::Navigator;
	using Navigation::PageId;

	struct GoalFixture
	{
		GoalFixture() :
			model(Navigation::CreateOneTouchMenuModel()),
			nav(model),
			page(12),
			device_id(JandyDeviceId(0x40))
		{
		}

		struct StepResult
		{
			GoalStatus status;
			std::optional<NavKeyCommand> key;
		};

		// Run one Status cycle of the goal against the current screen + cursor.
		StepResult Step(IKeypadGoal& goal)
		{
			KeypadContext ctx{ device_id, page, cursor, nav };
			const auto status = goal.Step(ctx);
			return { status, ctx.emitted_key };
		}

		void SetLine(uint8_t line, const std::string& text)
		{
			page[line].Text = text;
		}

		void ClearPage()
		{
			for (std::size_t i = 0; i < page.Size(); ++i)
			{
				page[i].Text.clear();
			}
		}

		// The two Status frames a page transition costs the Navigator after a Select.
		void StatusFrames(uint32_t count)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				nav.OnStatusMessageReceived();
			}
		}

		// Recognised pages (detector strings from navigation/onetouch_menu_model.cpp).
		void ShowServicePage()
		{
			ClearPage();
			SetLine(3, "  Service Mode  ");
		}

		void ShowEquipmentOnOffPage()
		{
			ClearPage();
			SetLine(0, "Filter Pump  ***");
			SetLine(1, "Spa           ON");
			SetLine(5, "Pool Light   OFF");
			SetLine(6, "Aux2         OFF");
			SetLine(11, "   ^^ More vv   ");
		}

		void ShowSetTemperaturePage(int pool_value)
		{
			ClearPage();
			SetLine(0, "    Set Temp    ");
			SetLine(2, "Pool Heat   " + std::to_string(pool_value) + "`F");
			SetLine(3, "Spa Heat   102`F");
		}

		void ShowBoostIdlePage()
		{
			ClearPage();
			SetLine(0, "   Boost Pool   ");
			SetLine(3, "  Operate the   ");
			SetLine(4, " chlorinator at ");
			SetLine(5, "      100%      ");
		}

		void ShowBoostRunningPage()
		{
			ClearPage();
			SetLine(0, "   Boost Pool   ");
			SetLine(2, " Time Remaining ");
			SetLine(3, "    23:15:00    ");
			SetLine(6, "Stop            ");
		}

		void ShowSystemSetupPage(bool with_spa_switch)
		{
			ClearPage();
			SetLine(0, "  System Setup  ");
			SetLine(2, "Label Aux      >");
			SetLine(3, "Freeze Protect >");
			if (with_spa_switch)
			{
				SetLine(5, "Spa Switch     >");
			}
		}

		void ShowSpaSwitchNumberPage()
		{
			ClearPage();
			SetLine(0, "   Spa Switch   ");
			SetLine(1, "     Setup      ");
			SetLine(3, "  1 Spa Switch  ");
		}

		void ShowButtonSetupPage(bool with_row)
		{
			ClearPage();
			SetLine(1, "  Button Setup  ");
			SetLine(3, "1:1  Spa        ");
			if (with_row)
			{
				SetLine(4, "1:2  Pool Light ");
			}
			SetLine(5, "1:3  Aux2       ");
		}

		void ShowPickerPage(const std::string& function)
		{
			ClearPage();
			SetLine(1, "   Button 1:2   ");
			SetLine(3, function);
		}

		void ShowProgramListPage(bool with_target)
		{
			ClearPage();
			SetLine(0, "    Program     ");
			SetLine(1, "    Group A     ");
			if (with_target)
			{
				SetLine(2, "Pool Light      ");
			}
			SetLine(3, "Aux2            ");
		}

		void ShowProgramDetailPage()
		{
			ClearPage();
			SetLine(0, "   Pool Light   ");
			SetLine(2, "   Pgm 1 of 1   ");
			SetLine(3, " ON     11:00 AM");
			SetLine(4, " OFF     2:00 PM");
			SetLine(5, " All Days       ");
			SetLine(9, "Add      Program");
			SetLine(10, "Delete   Program");
			SetLine(11, "Change   Program");
		}

		void ShowEmptyProgramDetailPage()
		{
			ClearPage();
			SetLine(0, "   Pool Light   ");
			SetLine(4, "  No Programs   ");
			SetLine(9, "  Add Program   ");
		}

		Navigation::MenuModel model;
		Navigator nav;
		Utility::ScreenDataPage page;
		JandyDeviceType device_id;
		uint8_t cursor{ Navigator::CURSOR_LINE_NONE };
	};

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

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_Goals, GoalFixture)

//=============================================================================
// KeypadContext::MoveCursorToward
//=============================================================================

BOOST_AUTO_TEST_CASE(MoveCursorToward_AlreadyOnTarget_NoKey)
{
	cursor = 4;
	KeypadContext ctx{ device_id, page, cursor, nav };
	BOOST_CHECK(ctx.MoveCursorToward(4));
	BOOST_CHECK(!ctx.emitted_key.has_value());
}

BOOST_AUTO_TEST_CASE(MoveCursorToward_NoCursor_EstablishesWithLineDown)
{
	cursor = Navigator::CURSOR_LINE_NONE;
	KeypadContext ctx{ device_id, page, cursor, nav };
	BOOST_CHECK(!ctx.MoveCursorToward(4));
	BOOST_REQUIRE(ctx.emitted_key.has_value());
	BOOST_CHECK(ctx.emitted_key.value() == NavKeyCommand::LineDown);
}

BOOST_AUTO_TEST_CASE(MoveCursorToward_StepsUpOrDown)
{
	cursor = 2;
	KeypadContext down{ device_id, page, cursor, nav };
	BOOST_CHECK(!down.MoveCursorToward(6));
	BOOST_REQUIRE(down.emitted_key.has_value());
	BOOST_CHECK(down.emitted_key.value() == NavKeyCommand::LineDown);

	cursor = 8;
	KeypadContext up{ device_id, page, cursor, nav };
	BOOST_CHECK(!up.MoveCursorToward(6));
	BOOST_REQUIRE(up.emitted_key.has_value());
	BOOST_CHECK(up.emitted_key.value() == NavKeyCommand::LineUp);
}

//=============================================================================
// OneTouchGoalRunner
//=============================================================================

BOOST_AUTO_TEST_CASE(Runner_OneGoalAtATime_AndFreesKeypadWhenDone)
{
	OneTouchGoalRunner runner;
	BOOST_CHECK(!runner.HasActiveGoal());

	// A goal that completes immediately (the Boost page already shows the boost running).
	BOOST_CHECK(runner.TryStart(std::make_unique<BoostGoal>(true)));
	BOOST_CHECK(runner.HasActiveGoal());
	BOOST_CHECK(!runner.TryStart(std::make_unique<BoostGoal>(false)));   // busy

	// Idle Service() with a goal still running keeps it.
	ShowBoostRunningPage();
	KeypadContext ctx{ device_id, page, cursor, nav };
	runner.Service(ctx);
	BOOST_CHECK(!runner.HasActiveGoal());   // Done -> cleared, navigator reset
	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);

	// No-op when idle.
	KeypadContext idle_ctx{ device_id, page, cursor, nav };
	BOOST_CHECK_NO_THROW(runner.Service(idle_ctx));
	BOOST_CHECK(!idle_ctx.emitted_key.has_value());
}

BOOST_AUTO_TEST_CASE(Runner_AbandonedGoal_IsCleared)
{
	OneTouchGoalRunner runner;
	BOOST_REQUIRE(runner.TryStart(std::make_unique<ToggleGoal>("Pool Light")));

	ShowServicePage();   // blocking page -> navigator fails -> goal abandoned
	KeypadContext ctx{ device_id, page, cursor, nav };
	runner.Service(ctx);
	BOOST_CHECK(!runner.HasActiveGoal());
}

//=============================================================================
// ToggleGoal
//=============================================================================

BOOST_AUTO_TEST_CASE(Toggle_CursorOnRow_SelectsThenCompletes)
{
	ToggleGoal goal("Pool Light");
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "toggle 'Pool Light'");

	ShowEquipmentOnOffPage();
	cursor = 5;

	// Cursor already on the row -> the navigator presses Select in place.
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// Page re-renders in place after two Status frames -> at destination -> Done.
	StatusFrames(2);
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(Toggle_CursorOffRow_MovesCursorFirst)
{
	ToggleGoal goal("Pool Light");

	ShowEquipmentOnOffPage();
	cursor = 1;

	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineDown);
}

BOOST_AUTO_TEST_CASE(Toggle_ServicePage_Fails)
{
	ToggleGoal goal("Pool Light");

	ShowServicePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

//=============================================================================
// ValueEditGoal (heater setpoint via the in-place editor)
//=============================================================================

BOOST_AUTO_TEST_CASE(ValueEdit_StepsUpToTargetAndCommits)
{
	ValueEditGoal goal(PageId::SetTemperature, 2, "Pool Heat", 82, "pool setpoint");
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "pool setpoint");

	ShowSetTemperaturePage(80);
	cursor = 2;

	// Navigating: cursor already on the row, no select target -> at destination, no key.
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// BeginEdit: 80 != 82 -> Select to enter the editor.
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// Stepping: 80 -> LineUp
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineUp);

	// 81 -> LineUp
	ShowSetTemperaturePage(81);
	r = Step(goal);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineUp);

	// 82 -> reached target, moves to Commit (no key this cycle)
	ShowSetTemperaturePage(82);
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// Commit: Select and Done.
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(ValueEdit_StepsDownWhenAboveTarget)
{
	ValueEditGoal goal(PageId::SetTemperature, 2, "Pool Heat", 78, "pool setpoint");

	ShowSetTemperaturePage(80);
	cursor = 2;

	Step(goal);                 // navigate (arrives)
	Step(goal);                 // BeginEdit -> Select
	auto r = Step(goal);        // Stepping: 80 > 78 -> LineDown
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineDown);
}

BOOST_AUTO_TEST_CASE(ValueEdit_AlreadyAtTarget_NoEditRequired)
{
	ValueEditGoal goal(PageId::SetTemperature, 2, "Pool Heat", 82, "pool setpoint");

	ShowSetTemperaturePage(82);
	cursor = 2;

	Step(goal);                 // navigate (arrives)
	auto r = Step(goal);        // BeginEdit: already 82 -> Done without touching the editor
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(ValueEdit_ValueBlankedMidRender_Waits)
{
	ValueEditGoal goal(PageId::SetTemperature, 2, "Pool Heat", 82, "pool setpoint");

	ShowSetTemperaturePage(80);
	cursor = 2;

	Step(goal);                 // navigate
	Step(goal);                 // BeginEdit -> Select

	// The row has no digits yet (mid-edit re-render): wait, no key.
	SetLine(2, "Pool Heat       ");
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(ValueEdit_NavigationFails_GoalFails)
{
	ValueEditGoal goal(PageId::SetTemperature, 2, "Pool Heat", 82, "pool setpoint");

	ShowServicePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

//=============================================================================
// BoostGoal
//=============================================================================

BOOST_AUTO_TEST_CASE(Boost_Start_OnIdlePage_SelectsThenSettles)
{
	BoostGoal goal(true);
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "chlorinator boost start");

	ShowBoostIdlePage();

	// Arrived on the Boost page, boost idle -> Select starts it.
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// Settle: one-shot, done.
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
}

BOOST_AUTO_TEST_CASE(Boost_Start_AlreadyRunning_NothingToDo)
{
	BoostGoal goal(true);

	ShowBoostRunningPage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(Boost_Stop_AlreadyStopped_NothingToDo)
{
	BoostGoal goal(false);
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "chlorinator boost stop");

	ShowBoostIdlePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(Boost_Stop_WhileRunning_NavigatesToStopAndSelects)
{
	BoostGoal goal(false);

	ShowBoostRunningPage();
	cursor = 6;   // on the "Stop" row

	// Arrived, boost running -> navigator retargeted to the "Stop" item (no key yet).
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// Acting: cursor already on "Stop" -> Select.
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// Two Status frames later the page has re-rendered: at destination -> Done.
	StatusFrames(2);
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
}

BOOST_AUTO_TEST_CASE(Boost_NavigationFails_GoalFails)
{
	BoostGoal goal(true);

	ShowServicePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

//=============================================================================
// SpaSwitchGoal
//=============================================================================

namespace
{
	// Drive a SpaSwitchGoal from a fresh start to the picker (phase CyclePicker), asserting
	// each emitted key along the way. Leaves the picker page showing 'first_function'.
	void DriveToPicker(GoalFixture& f, SpaSwitchGoal& goal, const std::string& first_function)
	{
		// ToSystemSetup: already on System Setup -> arrives (no key), hands off to screen walk.
		f.ShowSystemSetupPage(true);
		f.cursor = 2;
		auto r = f.Step(goal);
		BOOST_REQUIRE(r.status == GoalStatus::Running);
		BOOST_REQUIRE(!r.key.has_value());

		// SelectSpaSwitch: "Spa Switch" is on line 5, cursor on 2 -> LineDown.
		r = f.Step(goal);
		BOOST_REQUIRE(r.status == GoalStatus::Running);
		BOOST_REQUIRE(r.key.has_value());
		BOOST_REQUIRE(r.key.value() == NavKeyCommand::LineDown);

		// Cursor now on the row -> Select.
		f.cursor = 5;
		r = f.Step(goal);
		BOOST_REQUIRE(r.key.has_value());
		BOOST_REQUIRE(r.key.value() == NavKeyCommand::Select);

		// PassNumberPage: bare Select on the number-of-switches page.
		f.ShowSpaSwitchNumberPage();
		f.cursor = Navigator::CURSOR_LINE_NONE;
		r = f.Step(goal);
		BOOST_REQUIRE(r.key.has_value());
		BOOST_REQUIRE(r.key.value() == NavKeyCommand::Select);

		// ToRow: Button Setup list, "1:2" row on line 4, no cursor -> LineDown establishes one.
		f.ShowButtonSetupPage(true);
		r = f.Step(goal);
		BOOST_REQUIRE(r.key.has_value());
		BOOST_REQUIRE(r.key.value() == NavKeyCommand::LineDown);

		// Cursor on the row -> Select opens the picker.
		f.cursor = 4;
		r = f.Step(goal);
		BOOST_REQUIRE(r.key.has_value());
		BOOST_REQUIRE(r.key.value() == NavKeyCommand::Select);

		f.ShowPickerPage(first_function);
	}
}

BOOST_AUTO_TEST_CASE(SpaSwitch_FullWalk_CyclesPickerAndCommits)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "spa-switch 1:2 -> 'Pool Light'");

	DriveToPicker(*this, goal, "      Spa       ");

	// CyclePicker: "Spa" != "Pool Light" -> LineUp cycles.
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineUp);

	// Target shown (case-insensitive) -> Commit next cycle.
	ShowPickerPage("   pool light   ");
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// Commit: Select and Done.
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(SpaSwitch_PickerWraps_FunctionNotOffered_Fails)
{
	SpaSwitchGoal goal(1, 2, "Waterfall");

	DriveToPicker(*this, goal, "      Spa       ");

	auto r = Step(goal);              // first seen: Spa -> LineUp
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::LineUp);

	ShowPickerPage("     Heater     ");
	r = Step(goal);                   // Heater -> LineUp
	BOOST_CHECK(r.status == GoalStatus::Running);

	ShowPickerPage("      Spa       ");
	r = Step(goal);                   // wrapped back to Spa -> not offered
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

BOOST_AUTO_TEST_CASE(SpaSwitch_PickerNotRenderedYet_Waits)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");

	DriveToPicker(*this, goal, "                ");   // function row blank

	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// Still transitioning (not the picker at all): wait.
	ClearPage();
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(SpaSwitch_NumberPageNotYetShown_Waits)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");

	ShowSystemSetupPage(true);
	cursor = 5;
	Step(goal);                      // arrive on System Setup
	auto r = Step(goal);             // cursor on Spa Switch -> Select
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// The number page has not rendered yet: nothing emitted.
	ClearPage();
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(SpaSwitch_MenuItemMissing_ScrollsThenFails)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");

	ShowSystemSetupPage(false);     // no "Spa Switch" item on this panel
	cursor = 2;
	Step(goal);                     // arrive

	// Scrolls LineDown looking for it, bounded by MAX_SCROLL (40); the 41st gives up.
	GoalStatus last = GoalStatus::Running;
	uint32_t line_downs = 0;
	for (uint32_t i = 0; i < 41 && last == GoalStatus::Running; ++i)
	{
		auto r = Step(goal);
		last = r.status;
		if (r.key.has_value() && r.key.value() == NavKeyCommand::LineDown) { ++line_downs; }
	}
	BOOST_CHECK(last == GoalStatus::Failed);
	BOOST_CHECK_EQUAL(line_downs, 41u);
}

BOOST_AUTO_TEST_CASE(SpaSwitch_ButtonRowMissing_ScrollsThenFails)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");

	ShowSystemSetupPage(true);
	cursor = 5;
	Step(goal);                     // arrive
	Step(goal);                     // Select Spa Switch
	ShowSpaSwitchNumberPage();
	Step(goal);                     // Select through the number page

	ShowButtonSetupPage(false);     // "1:2" row absent
	GoalStatus last = GoalStatus::Running;
	for (uint32_t i = 0; i < 41 && last == GoalStatus::Running; ++i)
	{
		last = Step(goal).status;
	}
	BOOST_CHECK(last == GoalStatus::Failed);
}

BOOST_AUTO_TEST_CASE(SpaSwitch_NavigationFails_GoalFails)
{
	SpaSwitchGoal goal(1, 2, "Pool Light");

	ShowServicePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

//=============================================================================
// ScheduleWriteGoal (screen-driven branches not reached by the device-level
// write tests: navigator key emission, detail-page fast transition, equipment
// not found, delete-with-no-programs, verify-gone on an unparseable page).
//=============================================================================

BOOST_AUTO_TEST_CASE(ScheduleWrite_NavigatorEmitsKeyOnTheWayToProgramMenu)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Create, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "create");
	BOOST_CHECK_EQUAL(std::string(goal.Description()), "create");

	// On the Menu/Help page with the cursor on "Program": the navigator Selects it.
	ClearPage();
	SetLine(0, "   Menu / Help  ");
	SetLine(1, "Help           >");
	SetLine(2, "Program        >");
	SetLine(3, "Set Temp       >");
	cursor = 2;

	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// The Program list renders after two Status frames -> navigator complete -> screen walk.
	StatusFrames(2);
	ShowProgramListPage(true);
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());
}

BOOST_AUTO_TEST_CASE(ScheduleWrite_DetailPageAlreadyRendered_SkipsToChooseAction)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Create, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "create");

	ShowProgramListPage(true);
	Step(goal);                      // arrive on the Program list

	// A fast transition: the detail page is already up when SelectEquipment runs.
	ShowEmptyProgramDetailPage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_CHECK(!r.key.has_value());

	// ChooseAction (Create): cursor on the Add row -> Select into the editor.
	cursor = 9;
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(ScheduleWrite_EquipmentNotInList_ScrollsThenFails)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Create, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "create");

	ShowProgramListPage(false);
	Step(goal);                      // arrive on the Program list

	GoalStatus last = GoalStatus::Running;
	uint32_t line_downs = 0;
	for (uint32_t i = 0; i < 41 && last == GoalStatus::Running; ++i)
	{
		auto r = Step(goal);
		last = r.status;
		if (r.key.has_value() && r.key.value() == NavKeyCommand::LineDown) { ++line_downs; }
	}
	BOOST_CHECK(last == GoalStatus::Failed);
	BOOST_CHECK_EQUAL(line_downs, 41u);
}

BOOST_AUTO_TEST_CASE(ScheduleWrite_Delete_NoPrograms_IsDone)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Delete, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "delete");

	ShowProgramListPage(true);
	Step(goal);                      // arrive on the list

	ShowEmptyProgramDetailPage();
	Step(goal);                      // detail already rendered -> ChooseAction
	auto r = Step(goal);             // nothing to delete
	BOOST_CHECK(r.status == GoalStatus::Done);
}

BOOST_AUTO_TEST_CASE(ScheduleWrite_Delete_VerifyGone_UnparseableDetail_IsDone)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Delete, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "delete");

	ShowProgramListPage(true);
	Step(goal);                      // arrive on the list

	ShowProgramDetailPage();
	Step(goal);                      // detail already rendered -> ChooseAction

	cursor = 10;                     // on the Delete row
	auto r = Step(goal);
	BOOST_REQUIRE(r.key.has_value());
	BOOST_CHECK(r.key.value() == NavKeyCommand::Select);

	// VerifyGone: page mid-transition (not a detail page) -> wait.
	ClearPage();
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Running);

	// A detail page that no longer parses as a program (rows blanked) -> Done.
	ClearPage();
	SetLine(0, "   Pool Light   ");
	SetLine(2, "   Pgm 0 of 0   ");
	r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Done);
}

BOOST_AUTO_TEST_CASE(ScheduleWrite_NavigationFails_GoalFails)
{
	ScheduleWriteGoal goal(ScheduleWriteOp::Edit, ProgramSpec("Pool Light", 11, 0, 14, 0, 0x7f), "edit");

	ShowServicePage();
	auto r = Step(goal);
	BOOST_CHECK(r.status == GoalStatus::Failed);
}

BOOST_AUTO_TEST_SUITE_END()
