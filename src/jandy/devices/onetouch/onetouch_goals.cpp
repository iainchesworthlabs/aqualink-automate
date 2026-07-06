#include <format>
#include <utility>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_goals.h"
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

}
// namespace AqualinkAutomate::Devices::OneTouch
