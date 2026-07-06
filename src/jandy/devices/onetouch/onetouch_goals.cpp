#include <format>
#include <utility>
#include <vector>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_goals.h"
#include "devices/onetouch/onetouch_schedule_parser.h"
#include "devices/onetouch/onetouch_screen_reader.h"
#include "formatters/jandy_device_formatters.h"
#include "navigation/menu_model.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices::OneTouch
{

	ToggleGoal::ToggleGoal(std::string label) :
		m_Label(std::move(label)),
		m_Desc(std::format("toggle '{}'", m_Label))
	{
	}

	GoalStatus ToggleGoal::Step(KeypadContext& ctx)
	{
		// Kick off the navigation goal once: drive to the Equipment ON/OFF page, find the row whose
		// label matches the target device, and Select it.
		if (!m_Started)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Beginning {} navigation", ctx.DeviceId(), m_Desc));
			ctx.navigator.NavigateToItem(Navigation::PageId::EquipmentOnOff, 0, m_Label, Navigation::PageId::EquipmentOnOff);
			m_Started = true;
			m_StepCount = 0;
		}

		// Advance the navigator against the current screen and queue any key it asks for.
		if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
		{
			ctx.Emit(nav_cmd.value());
		}

		if (++m_StepCount > STEP_LIMIT)
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		if (ctx.navigator.IsComplete())
		{
			return ctx.navigator.IsSuccess() ? GoalStatus::Done : GoalStatus::Failed;
		}

		return GoalStatus::Running;
	}

	ValueEditGoal::ValueEditGoal(Navigation::PageId page, uint8_t line, std::string label, int target, std::string desc) :
		m_Page(page),
		m_Line(line),
		m_Label(std::move(label)),
		m_Target(target),
		m_Desc(std::move(desc))
	{
	}

	GoalStatus ValueEditGoal::Step(KeypadContext& ctx)
	{
		// Frame backstop so a mis-detected page can never wedge NormalOperation (the Navigator's
		// own timeouts normally drive it to Failed first).
		if (m_Started)
		{
			++m_StepCount;
		}

		if (m_Started && (m_StepCount > STEP_LIMIT))
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} edit exceeded {} steps - abandoning", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		switch (m_Phase)
		{
		case Phase::Navigating:
		{
			// Kick off navigation once: drive to the goal's page and position the cursor on the
			// value row. select_target is left Unknown so the Navigator stops AT the row (cursor
			// positioned) instead of pressing Select - the in-place value editor is driven here.
			if (!m_Started)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to '{}' row for {}", ctx.DeviceId(), m_Label, m_Desc));
				ctx.navigator.NavigateToItem(m_Page, m_Line, m_Label, Navigation::PageId::Unknown);
				m_Started = true;
				m_StepCount = 0;
			}

			if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
			{
				ctx.Emit(nav_cmd.value());
			}

			if (ctx.navigator.IsComplete())
			{
				if (ctx.navigator.IsSuccess())
				{
					LogInfo(Channel::Devices, std::format("OneTouch ({}): Cursor on '{}' row - entering value editor", ctx.DeviceId(), m_Label));
					m_Phase = Phase::BeginEdit;
				}
				else
				{
					return GoalStatus::Failed;
				}
			}
			break;
		}

		case Phase::BeginEdit:
		{
			// Skip the edit entirely if the row already shows the target value (avoids a pointless
			// enter/exit-edit toggle). Wait if the value isn't readable yet.
			if (auto current = DisplayedValue(ctx.page, m_Line); current.has_value() && (current.value() == m_Target))
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): '{}' already at target {} - no edit required", ctx.DeviceId(), m_Label, m_Target));
				return GoalStatus::Done;
			}

			// Select on the highlighted row ENTERS the in-place value editor (verified vs hardware:
			// Select then arrows change the value).
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to begin editing '{}'", ctx.DeviceId(), m_Label));
			ctx.Emit(Navigation::NavKeyCommand::Select);
			m_Phase = Phase::Stepping;
			break;
		}

		case Phase::Stepping:
		{
			// In edit mode, step toward the target per status cycle: LineUp increments, LineDown
			// decrements (the device applies its own increment - 1 degree for setpoints, 5% for
			// chlorinator). If the value isn't parseable yet (page mid-render), wait for the next.
			auto current = DisplayedValue(ctx.page, m_Line);
			if (!current.has_value())
			{
				LogTrace(Channel::Devices, std::format("OneTouch ({}): '{}' value not yet readable - waiting", ctx.DeviceId(), m_Label));
				break;
			}

			if (current.value() == m_Target)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): '{}' reached target {} - committing", ctx.DeviceId(), m_Label, m_Target));
				m_Phase = Phase::Commit;
				break;
			}

			ctx.Emit((current.value() < m_Target) ? Navigation::NavKeyCommand::LineUp : Navigation::NavKeyCommand::LineDown);
			LogTrace(Channel::Devices, std::format("OneTouch ({}): Stepping '{}' {} -> {}", ctx.DeviceId(), m_Label, current.value(), m_Target));
			break;
		}

		case Phase::Commit:
		{
			// Press Select once to COMMIT the edited value and leave the editor (verified vs
			// hardware: each edit is bracketed Select...Select, NOT Back).
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to commit '{}'", ctx.DeviceId(), m_Label));
			ctx.Emit(Navigation::NavKeyCommand::Select);
			return GoalStatus::Done;
		}
		}

		return GoalStatus::Running;
	}

	BoostGoal::BoostGoal(bool start) :
		m_Start(start),
		m_Desc(std::format("chlorinator boost {}", start ? "start" : "stop"))
	{
	}

	bool BoostGoal::BoostIsRunning(const KeypadContext& ctx)
	{
		// The Boost Pool page shows "Time Remaining" while a boost is running and "Operate ... at
		// 100%" when idle - used to decide whether an action is actually needed.
		for (std::size_t i = 0; i < ctx.page.Size(); ++i)
		{
			if (ctx.page[i].Text.contains("Time Remaining"))
			{
				return true;
			}
		}
		return false;
	}

	std::optional<GoalStatus> BoostGoal::HandleNavigating(KeypadContext& ctx)
	{
		// Drive to the Boost Pool page (no in-place Select yet - we decide the action from the
		// page state once there).
		if (!m_Started)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to Boost Pool to {} boost", ctx.DeviceId(), m_Start ? "start" : "stop"));
			ctx.navigator.NavigateTo(Navigation::PageId::Boost);
			m_Started = true;
			m_StepCount = 0;
		}

		if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
		{
			ctx.Emit(nav_cmd.value());
		}

		if (ctx.navigator.IsComplete())
		{
			if (!ctx.navigator.IsSuccess())
			{
				return GoalStatus::Failed;
			}

			const bool running = BoostIsRunning(ctx);
			if (m_Start && running)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): boost already running - nothing to do", ctx.DeviceId()));
				return GoalStatus::Done;
			}
			else if (!m_Start && !running)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): boost already stopped - nothing to do", ctx.DeviceId()));
				return GoalStatus::Done;
			}
			else if (m_Start)
			{
				// Idle page ("Operate the chlorinator at 100%"): a single Select starts boost.
				LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to start boost", ctx.DeviceId()));
				ctx.Emit(Navigation::NavKeyCommand::Select);
				m_Phase = Phase::Settle;
			}
			else
			{
				// Running page: navigate to the "Stop" submenu item and Select it in place.
				LogDebug(Channel::Devices, std::format("OneTouch ({}): Navigating to 'Stop' to stop boost", ctx.DeviceId()));
				ctx.navigator.NavigateToItem(Navigation::PageId::Boost, 0, "Stop", Navigation::PageId::Boost);
				m_Phase = Phase::Acting;
			}
		}
		return std::nullopt;
	}

	GoalStatus BoostGoal::Step(KeypadContext& ctx)
	{
		if (m_Started)
		{
			++m_StepCount;
		}

		if (m_Started && (m_StepCount > STEP_LIMIT))
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		switch (m_Phase)
		{
		case Phase::Navigating:
		{
			if (auto s = HandleNavigating(ctx); s.has_value())
			{
				return s.value();
			}
			break;
		}

		case Phase::Acting:
		{
			// Stop path: let the Navigator walk the cursor to the "Stop" item and Select it.
			if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
			{
				ctx.Emit(nav_cmd.value());
			}
			if (ctx.navigator.IsComplete())
			{
				return ctx.navigator.IsSuccess() ? GoalStatus::Done : GoalStatus::Failed;
			}
			break;
		}

		case Phase::Settle:
		{
			// Start path: the Select has been queued; the action is one-shot, so we are done.
			return GoalStatus::Done;
		}
		}

		return GoalStatus::Running;
	}

	SpaSwitchGoal::SpaSwitchGoal(uint8_t switch_number, uint8_t button_number, std::string function) :
		m_SwitchNumber(switch_number),
		m_ButtonNumber(button_number),
		m_Function(std::move(function)),
		m_RowTag(std::format("{}:{}", switch_number, button_number)),
		m_Desc(std::format("spa-switch {}:{} -> '{}'", switch_number, button_number, m_Function))
	{
	}

	std::optional<GoalStatus> SpaSwitchGoal::HandleToSystemSetup(KeypadContext& ctx)
	{
		// Reuse the proven navigator to reach System Setup, then hand off to the screen-driven
		// walk (the Spa Switch sub-pages -- especially the number-of-switches page -- need bare
		// Selects without cursor moves, which the navigator's edge model does not express).
		if (!m_Started)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to System Setup for {}", ctx.DeviceId(), m_Desc));
			ctx.navigator.NavigateTo(Navigation::PageId::SystemSetup);
			m_Started = true;
			m_StepCount = 0;
		}

		if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
		{
			ctx.Emit(nav_cmd.value());
		}

		if (ctx.navigator.IsComplete())
		{
			if (ctx.navigator.IsSuccess())
			{
				ctx.navigator.Reset();   // navigation done; the rest is screen-driven
				m_CursorStuck = 0;
				m_Phase = Phase::SelectSpaSwitch;
			}
			else
			{
				return GoalStatus::Failed;
			}
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> SpaSwitchGoal::HandleSelectSpaSwitch(KeypadContext& ctx)
	{
		// On the System Setup menu: find the "Spa Switch" item (scrolling if below the fold),
		// move the cursor onto it, then Select to open the Spa Switch number page.
		if (auto line = FindLineStartingWith(ctx.page, "Spa Switch"); line.has_value())
		{
			m_CursorStuck = 0;
			if (ctx.MoveCursorToward(line.value()))
			{
				ctx.Emit(Navigation::NavKeyCommand::Select);
				m_Phase = Phase::PassNumberPage;
			}
		}
		else
		{
			ctx.Emit(Navigation::NavKeyCommand::LineDown);   // scroll the list to reveal it
			if (++m_CursorStuck > MAX_SCROLL)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): 'Spa Switch' menu item not found", ctx.DeviceId()));
				return GoalStatus::Failed;
			}
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> SpaSwitchGoal::HandlePassNumberPage(KeypadContext& ctx)
	{
		// The "Spa Switch / Setup" number-of-switches page (line 1 == "Setup"). Press a BARE
		// Select to advance to the Button Setup list WITHOUT moving the cursor -- moving it
		// would change the configured switch count.
		if (LineText(ctx.page, 1) == "Setup")
		{
			ctx.Emit(Navigation::NavKeyCommand::Select);
			m_Phase = Phase::ToRow;
			m_CursorStuck = 0;
		}
		return std::nullopt;   // else: still transitioning -- wait for the page
	}

	std::optional<GoalStatus> SpaSwitchGoal::HandleToRow(KeypadContext& ctx)
	{
		// The "Button Setup" list (line 1 contains "Button Setup"). Find the "S:B" row, move
		// the cursor onto it, Select to open that button's function picker.
		if (LineText(ctx.page, 1).contains("Button Setup"))
		{
			if (auto line = FindLineStartingWith(ctx.page, m_RowTag); line.has_value())
			{
				m_CursorStuck = 0;
				if (ctx.MoveCursorToward(line.value()))
				{
					ctx.Emit(Navigation::NavKeyCommand::Select);
					m_PickerFirstSeen.reset();
					m_Phase = Phase::CyclePicker;
				}
			}
			else
			{
				ctx.Emit(Navigation::NavKeyCommand::LineDown);   // scroll to reveal the row
				if (++m_CursorStuck > MAX_SCROLL)
				{
					LogWarning(Channel::Devices, std::format("OneTouch ({}): button row '{}' not found", ctx.DeviceId(), m_RowTag));
					return GoalStatus::Failed;
				}
			}
		}
		return std::nullopt;   // else: still transitioning -- wait
	}

	std::optional<GoalStatus> SpaSwitchGoal::HandleCyclePicker(KeypadContext& ctx)
	{
		// The per-button picker (line 1 == "Button <S:B>"). Cycle (LineUp) until the selected
		// function (line 3) matches the target, then commit. Wrap-detect to bail if the target
		// is not offered by this controller.
		if (LineText(ctx.page, 1).contains("Button") && LineText(ctx.page, 1).contains(m_RowTag))
		{
			auto current = DisplayedFunctionOnRow(ctx.page, PICKER_FUNCTION_LINE);
			if (!current.has_value())
			{
				return std::nullopt;   // not rendered yet -- wait
			}

			if (EqualsCaseInsensitive(current.value(), m_Function))
			{
				m_Phase = Phase::Commit;
				return std::nullopt;
			}

			if (!m_PickerFirstSeen.has_value())
			{
				m_PickerFirstSeen = current;
			}
			else if (EqualsCaseInsensitive(current.value(), m_PickerFirstSeen.value()))
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): function '{}' not offered by the picker for {}", ctx.DeviceId(), m_Function, m_RowTag));
				return GoalStatus::Failed;
			}

			ctx.Emit(Navigation::NavKeyCommand::LineUp);   // cycle to the next function
		}
		return std::nullopt;   // else: not on the picker yet -- wait
	}

	GoalStatus SpaSwitchGoal::Step(KeypadContext& ctx)
	{
		if (m_Started)
		{
			++m_StepCount;
		}

		if (m_Started && (m_StepCount > STEP_LIMIT))
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		switch (m_Phase)
		{
		case Phase::ToSystemSetup:
			if (auto s = HandleToSystemSetup(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::SelectSpaSwitch:
			if (auto s = HandleSelectSpaSwitch(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::PassNumberPage:
			if (auto s = HandlePassNumberPage(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::ToRow:
			if (auto s = HandleToRow(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::CyclePicker:
			if (auto s = HandleCyclePicker(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::Commit:
		{
			// Select writes the chosen function and leaves the picker (back to the Button Setup
			// list, which then shows "S:B  <function>").
			ctx.Emit(Navigation::NavKeyCommand::Select);
			return GoalStatus::Done;
		}
		}

		return GoalStatus::Running;
	}

	//=========================================================================
	// Controller-schedule WRITE. RE'd from captures/onetouch_program.cap; see
	// docs/onetouch_schedule_protocol.md (write path).
	//=========================================================================

	ScheduleWriteGoal::ScheduleWriteGoal(ScheduleWriteOp op, Scheduling::ControllerSchedule program, std::string desc) :
		m_Op(op),
		m_Program(std::move(program)),
		m_Desc(std::move(desc))
	{
	}

	bool ScheduleWriteGoal::OnDetailPage(const KeypadContext& ctx) const
	{
		// The LIST's line 0 is the "Program" title; the detail page's line 0 is the equipment name
		// and it carries either "Pgm N of M" (line 2), "No Programs" (line 4), or a Change row.
		if (LineText(ctx.page, 0).contains("Program")) { return false; }
		return LineText(ctx.page, 2).contains("Pgm ")
			|| LineText(ctx.page, 4).contains("No Programs")
			|| LineText(ctx.page, CHANGE_ROW).contains("Change");
	}

	bool ScheduleWriteGoal::OnEditorPage(const KeypadContext& ctx) const
	{
		const std::string title = LineText(ctx.page, TITLE_LINE);
		return title.contains("New Program") || title.contains("Change Program");
	}

	std::optional<std::pair<int, int>> ScheduleWriteGoal::DisplayedTime(const KeypadContext& ctx, uint8_t line) const
	{
		if (line >= ctx.page.Size())
		{
			return std::nullopt;
		}

		// Reuse the read-path parser: hand the line to ParseProgramDetailLines shaped as a minimal
		// detail page and read back the field we asked for (ONE 12h->24h decode, no duplicate here).
		std::vector<std::string> lines(6, std::string{});
		lines[0] = "X";              // non-empty target so the parse is not rejected
		lines[3] = "ON  1:00 AM";    // placeholder for the row we are NOT reading
		lines[4] = "OFF 1:00 AM";
		lines[5] = "All Days";
		const uint8_t slot = (line == OFF_LINE) ? 4 : 3;
		lines[slot] = ctx.page[line].Text;

		const auto parsed = ParseProgramDetailLines(lines);
		if (!parsed.has_value())
		{
			return std::nullopt;
		}
		return (slot == 4)
			? std::pair<int, int>{ parsed->off_hour, parsed->off_minute }
			: std::pair<int, int>{ parsed->on_hour, parsed->on_minute };
	}

	std::optional<uint8_t> ScheduleWriteGoal::DisplayedDays(const KeypadContext& ctx, uint8_t line) const
	{
		if (line >= ctx.page.Size())
		{
			return std::nullopt;
		}
		return ParseDaysRow(ctx.page[line].Text);
	}

	std::optional<GoalStatus> ScheduleWriteGoal::StepHour(KeypadContext& ctx, uint8_t line, int target_hour, Phase next)
	{
		// Step a 12h+meridiem hour wheel (24 positions) toward target_hour closed-loop: read the
		// echoed value, emit ONE key the shorter way round, Select once matched. Advances to next.
		if (!OnEditorPage(ctx)) { return std::nullopt; }   // page mid-transition; wait
		auto cur = DisplayedTime(ctx, line);
		if (!cur.has_value()) { return std::nullopt; }     // value blanked mid-render; wait
		if (cur->first == target_hour)
		{
			ctx.Emit(Navigation::NavKeyCommand::Select);   // commit the hour, advance to the minute
			m_Phase = next;
			m_FieldStep = 0;
			return std::nullopt;
		}
		if (++m_FieldStep > MAX_STEP) { return GoalStatus::Failed; }
		const int forward = ((target_hour - cur->first) + 24) % 24;   // steps if we go LineUp (+1/step)
		ctx.Emit((forward <= 12) ? Navigation::NavKeyCommand::LineUp : Navigation::NavKeyCommand::LineDown);
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::StepMinute(KeypadContext& ctx, uint8_t line, int target_minute, Phase next)
	{
		// Step a 0-59 minute wheel toward target_minute closed-loop, then Select to advance.
		if (!OnEditorPage(ctx)) { return std::nullopt; }
		auto cur = DisplayedTime(ctx, line);
		if (!cur.has_value()) { return std::nullopt; }
		if (cur->second == target_minute)
		{
			ctx.Emit(Navigation::NavKeyCommand::Select);
			m_Phase = next;
			m_FieldStep = 0;
			return std::nullopt;
		}
		if (++m_FieldStep > MAX_STEP) { return GoalStatus::Failed; }
		const int forward = ((target_minute - cur->second) + 60) % 60;
		ctx.Emit((forward <= 30) ? Navigation::NavKeyCommand::LineUp : Navigation::NavKeyCommand::LineDown);
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleToProgramMenu(KeypadContext& ctx)
	{
		// Reuse the proven navigator to reach the Program equipment-list page, then hand off to
		// the screen-driven walk (the list scroll + detail + editor need bare content-driven
		// cursoring the navigator's edge model does not express).
		if (!m_Started)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to Program menu for {}", ctx.DeviceId(), m_Desc));
			ctx.navigator.NavigateTo(Navigation::PageId::Program);
			m_Started = true;
			m_StepCount = 0;
		}

		if (auto nav_cmd = ctx.navigator.OnPageUpdate(ctx.page, ctx.highlighted_line); nav_cmd.has_value())
		{
			ctx.Emit(nav_cmd.value());
		}

		if (ctx.navigator.IsComplete())
		{
			if (ctx.navigator.IsSuccess())
			{
				ctx.navigator.Reset();   // navigation done; the rest is screen-driven
				m_FieldStep = 0;
				m_Phase = Phase::SelectEquipment;
			}
			else
			{
				return GoalStatus::Failed;
			}
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleSelectEquipment(KeypadContext& ctx)
	{
		// On the Program equipment LIST: find the target equipment row (scrolling if below the
		// fold), move the cursor onto it, Select -> its detail page. Guard against acting once
		// the detail page has already rendered (a fast transition).
		if (OnDetailPage(ctx))
		{
			m_FieldStep = 0;
			m_Phase = Phase::ChooseAction;
			return GoalStatus::Running;
		}
		if (auto line = FindLineStartingWith(ctx.page, m_Program.target); line.has_value() && (line.value() != 0))
		{
			m_FieldStep = 0;
			if (ctx.MoveCursorToward(line.value()))
			{
				ctx.Emit(Navigation::NavKeyCommand::Select);
				m_Phase = Phase::ChooseAction;
			}
		}
		else
		{
			ctx.Emit(Navigation::NavKeyCommand::LineDown);   // scroll the list to reveal the equipment
			if (++m_FieldStep > MAX_STEP)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): equipment '{}' not found in the Program list", ctx.DeviceId(), m_Program.target));
				return GoalStatus::Failed;
			}
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleChooseAction(KeypadContext& ctx)
	{
		// On the per-equipment detail page. Delete: cursor to the Delete row and Select ->
		// immediate removal (NO confirm). Create: cursor to Add; Edit: cursor to Change -> editor.
		if (!OnDetailPage(ctx)) { return std::nullopt; }   // still transitioning -- wait

		if (m_Op == ScheduleWriteOp::Delete)
		{
			if (LineText(ctx.page, 4).contains("No Programs"))
			{
				return GoalStatus::Done;   // nothing to delete -- treat as done
			}
			if (ctx.MoveCursorToward(DELETE_ROW))
			{
				ctx.Emit(Navigation::NavKeyCommand::Select);   // immediate delete, no confirm
				m_Phase = Phase::VerifyGone;
			}
			return std::nullopt;
		}

		if (const uint8_t action_row = (m_Op == ScheduleWriteOp::Edit) ? CHANGE_ROW : ADD_ROW; ctx.MoveCursorToward(action_row))
		{
			ctx.Emit(Navigation::NavKeyCommand::Select);   // -> the editor
			m_FieldStep = 0;
			m_Phase = Phase::EnterEditor;
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleSetDays(KeypadContext& ctx)
	{
		// Step the days wheel to the target selection, then Select -> the program SAVES and the
		// panel returns to the detail page. Closed-loop on the echoed days row; a validated
		// candidate (CheckControllerCandidate) is always reachable.
		if (!OnEditorPage(ctx)) { return std::nullopt; }
		auto cur = DisplayedDays(ctx, DAYS_LINE);
		if (!cur.has_value()) { return std::nullopt; }   // days row blanked mid-render; wait
		if (cur.value() == (m_Program.days_of_week & DayMask::AllDays))
		{
			ctx.Emit(Navigation::NavKeyCommand::Select);   // commit days -> SAVE -> detail page
			m_Phase = Phase::Verify;
			m_FieldStep = 0;
			return std::nullopt;
		}
		if (++m_FieldStep > MAX_STEP) { return GoalStatus::Failed; }
		ctx.Emit(Navigation::NavKeyCommand::LineUp);   // cycle the days wheel (bounded)
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleVerify(KeypadContext& ctx)
	{
		// The program saved and the panel returned to the detail page. Re-parse it and confirm it
		// now carries the target program (target + on/off + days). Dwell until it renders.
		if (!OnDetailPage(ctx)) { return std::nullopt; }
		int idx = 0;
		int count = 0;
		if (const auto parsed = ParseProgramDetailPage(ctx.page, &idx, &count);
			parsed.has_value()
			&& EqualsCaseInsensitive(parsed->target, m_Program.target)
			&& parsed->days_of_week == (m_Program.days_of_week & DayMask::AllDays)
			&& parsed->on_hour == m_Program.on_hour && parsed->on_minute == m_Program.on_minute
			&& parsed->off_hour == m_Program.off_hour && parsed->off_minute == m_Program.off_minute)
		{
			return GoalStatus::Done;
		}
		return std::nullopt;
	}

	std::optional<GoalStatus> ScheduleWriteGoal::HandleVerifyGone(KeypadContext& ctx)
	{
		// Delete complete once the detail page shows "No Programs" (or no longer parses as a
		// program-detail). Dwell until the panel re-renders.
		if (!OnDetailPage(ctx)) { return std::nullopt; }
		if (LineText(ctx.page, 4).contains("No Programs"))
		{
			return GoalStatus::Done;
		}
		if (const auto parsed = ParseProgramDetailPage(ctx.page); !parsed.has_value())
		{
			return GoalStatus::Done;
		}
		return std::nullopt;
	}

	GoalStatus ScheduleWriteGoal::Step(KeypadContext& ctx)
	{
		// Frame backstop so a mis-detected page can never wedge NormalOperation.
		if (m_Started)
		{
			++m_StepCount;
		}

		if (m_Started && (m_StepCount > STEP_LIMIT))
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		switch (m_Phase)
		{
		case Phase::ToProgramMenu:
			if (auto s = HandleToProgramMenu(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::SelectEquipment:
			if (auto s = HandleSelectEquipment(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::ChooseAction:
			if (auto s = HandleChooseAction(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::EnterEditor:
		{
			// Wait for the Add/Change editor to render, then begin field entry at ON-hour. The panel
			// reports NO field highlight, so the active field is tracked purely by phase progression.
			if (OnEditorPage(ctx))
			{
				m_FieldStep = 0;
				m_Phase = Phase::SetOnHour;
			}
			break;
		}

		case Phase::SetOnHour:
			if (auto s = StepHour(ctx, ON_LINE, m_Program.on_hour, Phase::SetOnMinute); s.has_value()) { return s.value(); }
			break;

		case Phase::SetOnMinute:
			if (auto s = StepMinute(ctx, ON_LINE, m_Program.on_minute, Phase::SetOffHour); s.has_value()) { return s.value(); }
			break;

		case Phase::SetOffHour:
			if (auto s = StepHour(ctx, OFF_LINE, m_Program.off_hour, Phase::SetOffMinute); s.has_value()) { return s.value(); }
			break;

		case Phase::SetOffMinute:
			if (auto s = StepMinute(ctx, OFF_LINE, m_Program.off_minute, Phase::SetDays); s.has_value()) { return s.value(); }
			break;

		case Phase::SetDays:
			if (auto s = HandleSetDays(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::Verify:
			if (auto s = HandleVerify(ctx); s.has_value()) { return s.value(); }
			break;

		case Phase::VerifyGone:
			if (auto s = HandleVerifyGone(ctx); s.has_value()) { return s.value(); }
			break;
		}

		return GoalStatus::Running;
	}

}
// namespace AqualinkAutomate::Devices::OneTouch
