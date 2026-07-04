#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "scheduling/controller_schedule.h"
#include "scheduling/schedule.h"

namespace AqualinkAutomate::Scheduling
{

	//=========================================================================
	// Promotion constraints — the feasibility rules for moving a schedule down
	// to the controller (see docs/schedules-design.md, "Moving between tiers").
	//
	// App schedules are rich (arbitrary days, setpoints, %, circulation, one-off);
	// the controller's built-in Program timers are a constrained subset: a single
	// equipment on/off SPAN on one of a fixed set of day selections. This module is
	// the pure predicate the promotion flow (and the "promotable?" UI hint) use to
	// decide whether a candidate fits — and, when it does not, exactly why.
	//
	// It reports WHY as stable enum codes rather than prose so the web UI can
	// localise them; the codes never move between the wire and a locale catalog.
	//=========================================================================

	// The only day selections the controller's Program menu can express (owner
	// manual 6.4: "all days, weekends, weekdays, or any specific day of the week").
	enum class ControllerDaySelection : std::uint8_t
	{
		AllDays,        // every day (0x7f)
		Weekdays,       // Mon-Fri (0x1f)
		Weekends,       // Sat+Sun (0x60)
		SingleDay,      // exactly one day
		NotExpressible, // no days, or an arbitrary multi-day combination (e.g. Mon+Wed+Fri)
	};

	std::string_view ControllerDaySelectionToString(ControllerDaySelection selection);

	// Classify a days-of-week bitmask (bit0 = Monday .. bit6 = Sunday) into the
	// controller's allowed selection. Anything the controller cannot pick from its
	// menu (zero days, or a combination that is not all/weekdays/weekends/one day)
	// returns NotExpressible.
	ControllerDaySelection ClassifyDaySelection(std::uint8_t days_of_week);

	// Reasons a candidate cannot be represented on the controller. Stable codes;
	// the UI maps each to a localised explanation.
	enum class PromotionBlocker : std::uint8_t
	{
		TargetMissing,              // no equipment/circuit to drive
		ActionNotOnOff,             // a setpoint / % / circulation / toggle -- the controller only times on/off
		DaySelectionNotExpressible, // an arbitrary day combination the controller cannot pick
		TimeInvalid,                // on/off time out of range
	};

	std::string_view PromotionBlockerToString(PromotionBlocker blocker);

	struct PromotionResult
	{
		bool promotable{ false };
		std::vector<PromotionBlocker> blockers;                                  // empty iff promotable
		ControllerDaySelection day_selection{ ControllerDaySelection::NotExpressible };
	};

	// Can this equipment on/off SPAN candidate be represented as a controller
	// Program? Checks a non-empty target, in-range on/off times, and an expressible
	// day selection. (The candidate is already span-shaped, so this does not judge
	// the action kind -- see IsControllerRepresentableAction for that gate.)
	PromotionResult CheckControllerCandidate(const ControllerSchedule& candidate);

	// Is this app action one the controller can represent at all? Only button_on /
	// button_off map to a controller equipment timer; toggle (no explicit edge),
	// setpoints, chlorinator %, and circulation mode have no controller-program
	// equivalent, so a schedule carrying them can never be promoted.
	bool IsControllerRepresentableAction(ActionType type);

}
// namespace AqualinkAutomate::Scheduling
