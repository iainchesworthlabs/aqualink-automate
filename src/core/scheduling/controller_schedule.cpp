#include <format>

#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Scheduling
{

	std::string_view ControllerScheduleStatusToString(ControllerScheduleStatus status)
	{
		switch (status)
		{
		case ControllerScheduleStatus::Available:      return "available";
		case ControllerScheduleStatus::PendingCapture: return "pending_capture";
		case ControllerScheduleStatus::Unsupported:    return "unsupported";
		}
		return "pending_capture";
	}

	nlohmann::json ToJson(const ControllerSchedule& schedule)
	{
		return nlohmann::json{
			{ "id", schedule.id },
			{ "name", schedule.name },
			{ "target", schedule.target },
			{ "group", schedule.group },
			{ "enabled", schedule.enabled },
			{ "days_of_week", schedule.days_of_week },
			{ "on_local", std::format("{:02d}:{:02d}", schedule.on_hour, schedule.on_minute) },
			{ "off_local", std::format("{:02d}:{:02d}", schedule.off_hour, schedule.off_minute) },
		};
	}

}
// namespace AqualinkAutomate::Scheduling
