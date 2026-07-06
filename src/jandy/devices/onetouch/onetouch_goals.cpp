#include <format>
#include <utility>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_goals.h"
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
		if (m_Started && (++m_StepCount > STEP_LIMIT))
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

	GoalStatus BoostGoal::Step(KeypadContext& ctx)
	{
		if (m_Started && (++m_StepCount > STEP_LIMIT))
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", ctx.DeviceId(), m_Desc, STEP_LIMIT));
			return GoalStatus::Failed;
		}

		// The Boost Pool page shows "Time Remaining" while a boost is running and "Operate ... at
		// 100%" when idle - used to decide whether an action is actually needed.
		auto boost_is_running = [&ctx]()
		{
			for (std::size_t i = 0; i < ctx.page.Size(); ++i)
			{
				if (ctx.page[i].Text.contains("Time Remaining"))
				{
					return true;
				}
			}
			return false;
		};

		switch (m_Phase)
		{
		case Phase::Navigating:
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

				const bool running = boost_is_running();
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

}
// namespace AqualinkAutomate::Devices::OneTouch
