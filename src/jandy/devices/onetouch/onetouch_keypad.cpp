#include <format>
#include <utility>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_keypad.h"
#include "formatters/jandy_device_formatters.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices::OneTouch
{

	bool KeypadContext::MoveCursorToward(uint8_t target_line)
	{
		if (highlighted_line == target_line)
		{
			return true;
		}
		if (highlighted_line == Navigation::Navigator::CURSOR_LINE_NONE)
		{
			Emit(Navigation::NavKeyCommand::LineDown);   // establish a cursor first
			return false;
		}
		Emit((highlighted_line < target_line) ? Navigation::NavKeyCommand::LineDown : Navigation::NavKeyCommand::LineUp);
		return false;
	}

	bool OneTouchGoalRunner::TryStart(std::unique_ptr<IKeypadGoal> goal)
	{
		if (nullptr != m_ActiveGoal)
		{
			return false;   // keypad busy - one goal at a time
		}
		m_ActiveGoal = std::move(goal);
		return true;
	}

	void OneTouchGoalRunner::Service(KeypadContext& ctx)
	{
		if (nullptr == m_ActiveGoal)
		{
			return;
		}

		const auto status = m_ActiveGoal->Step(ctx);
		if (GoalStatus::Running == status)
		{
			return;
		}

		if (GoalStatus::Done == status)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): {} completed", ctx.DeviceId(), m_ActiveGoal->Description()));
		}
		else
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): {} abandoned", ctx.DeviceId(), m_ActiveGoal->Description()));
		}

		// The keypad is free again: reset the shared Navigator so the next goal starts clean.
		ctx.navigator.Reset();
		m_ActiveGoal.reset();
	}

}
// namespace AqualinkAutomate::Devices::OneTouch
