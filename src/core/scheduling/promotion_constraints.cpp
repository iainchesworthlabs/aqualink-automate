#include "scheduling/promotion_constraints.h"

#include <bit>

namespace AqualinkAutomate::Scheduling
{

	namespace
	{
		constexpr std::uint8_t DAYS_ALL      = 0x7f;
		constexpr std::uint8_t DAYS_WEEKDAYS = 0x1f; // Mon-Fri (bits 0..4)
		constexpr std::uint8_t DAYS_WEEKENDS = 0x60; // Sat+Sun (bits 5,6)

		bool TimeInRange(int hour, int minute)
		{
			return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
		}
	}

	std::string_view ControllerDaySelectionToString(ControllerDaySelection selection)
	{
		using enum ControllerDaySelection;
		switch (selection)
		{
		case AllDays:        return "all_days";
		case Weekdays:       return "weekdays";
		case Weekends:       return "weekends";
		case SingleDay:      return "single_day";
		case NotExpressible: return "not_expressible";
		}
		return "not_expressible";
	}

	ControllerDaySelection ClassifyDaySelection(std::uint8_t days_of_week)
	{
		using enum ControllerDaySelection;

		const std::uint8_t days = days_of_week & DAYS_ALL; // ignore any bits above the 7 weekdays

		if (days == 0)             { return NotExpressible; }
		if (days == DAYS_ALL)      { return AllDays; }
		if (days == DAYS_WEEKDAYS) { return Weekdays; }
		if (days == DAYS_WEEKENDS) { return Weekends; }
		if (std::popcount(days) == 1) { return SingleDay; }

		return NotExpressible;
	}

	std::string_view PromotionBlockerToString(PromotionBlocker blocker)
	{
		using enum PromotionBlocker;
		switch (blocker)
		{
		case TargetMissing:              return "target_missing";
		case ActionNotOnOff:             return "action_not_on_off";
		case DaySelectionNotExpressible: return "day_selection_not_expressible";
		case TimeInvalid:                return "time_invalid";
		}
		return "target_missing";
	}

	PromotionResult CheckControllerCandidate(const ControllerSchedule& candidate)
	{
		PromotionResult result;
		result.day_selection = ClassifyDaySelection(candidate.days_of_week);

		if (candidate.target.empty())
		{
			result.blockers.push_back(PromotionBlocker::TargetMissing);
		}
		if (!TimeInRange(candidate.on_hour, candidate.on_minute) ||
			!TimeInRange(candidate.off_hour, candidate.off_minute))
		{
			result.blockers.push_back(PromotionBlocker::TimeInvalid);
		}
		if (result.day_selection == ControllerDaySelection::NotExpressible)
		{
			result.blockers.push_back(PromotionBlocker::DaySelectionNotExpressible);
		}

		result.promotable = result.blockers.empty();
		return result;
	}

	bool IsControllerRepresentableAction(ActionType type)
	{
		return type == ActionType::ButtonOn || type == ActionType::ButtonOff;
	}

	std::optional<ControllerSchedule> BuildPromotionCandidate(const Schedule& on_schedule, const Schedule& off_schedule, std::string& error)
	{
		if (on_schedule.action.type != ActionType::ButtonOn)
		{
			error = "the ON schedule must be a button_on action";
			return std::nullopt;
		}
		if (off_schedule.action.type != ActionType::ButtonOff)
		{
			error = "the OFF schedule must be a button_off action";
			return std::nullopt;
		}
		if (on_schedule.action.target.empty() || on_schedule.action.target != off_schedule.action.target)
		{
			error = "the ON and OFF schedules must drive the same target device";
			return std::nullopt;
		}
		if (on_schedule.days_of_week != off_schedule.days_of_week)
		{
			error = "the ON and OFF schedules must run on the same days";
			return std::nullopt;
		}

		ControllerSchedule candidate;
		candidate.target = on_schedule.action.target;
		candidate.days_of_week = on_schedule.days_of_week;
		candidate.on_hour = on_schedule.hour;
		candidate.on_minute = on_schedule.minute;
		candidate.off_hour = off_schedule.hour;
		candidate.off_minute = off_schedule.minute;
		candidate.enabled = true;
		return candidate;
	}

}
// namespace AqualinkAutomate::Scheduling
