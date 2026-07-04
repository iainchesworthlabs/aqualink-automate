#include <algorithm>

#include <boost/test/unit_test.hpp>

#include "scheduling/controller_schedule.h"
#include "scheduling/promotion_constraints.h"
#include "scheduling/schedule.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Scheduling;

namespace
{
	bool HasBlocker(const PromotionResult& r, PromotionBlocker b)
	{
		return std::find(r.blockers.begin(), r.blockers.end(), b) != r.blockers.end();
	}

	ControllerSchedule ValidCandidate()
	{
		ControllerSchedule c;
		c.target = "Filter Pump";
		c.days_of_week = 0x7f; // all days
		c.on_hour = 9;  c.on_minute = 0;
		c.off_hour = 17; c.off_minute = 0;
		return c;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_PromotionConstraints)

//=============================================================================
// Day-selection classification — the controller's core limitation.
//=============================================================================

BOOST_AUTO_TEST_CASE(Days_AllowedSelections)
{
	BOOST_CHECK(ClassifyDaySelection(0x7f) == ControllerDaySelection::AllDays);
	BOOST_CHECK(ClassifyDaySelection(0x1f) == ControllerDaySelection::Weekdays);  // Mon-Fri
	BOOST_CHECK(ClassifyDaySelection(0x60) == ControllerDaySelection::Weekends);  // Sat+Sun
}

BOOST_AUTO_TEST_CASE(Days_EachSingleDay)
{
	for (int bit = 0; bit < 7; ++bit)
	{
		BOOST_CHECK(ClassifyDaySelection(static_cast<std::uint8_t>(1u << bit)) == ControllerDaySelection::SingleDay);
	}
}

BOOST_AUTO_TEST_CASE(Days_ArbitraryCombosNotExpressible)
{
	BOOST_CHECK(ClassifyDaySelection(0x15) == ControllerDaySelection::NotExpressible); // Mon+Wed+Fri
	BOOST_CHECK(ClassifyDaySelection(0x03) == ControllerDaySelection::NotExpressible); // Mon+Tue
	BOOST_CHECK(ClassifyDaySelection(0x41) == ControllerDaySelection::NotExpressible); // Mon+Sun
	BOOST_CHECK(ClassifyDaySelection(0x00) == ControllerDaySelection::NotExpressible); // no days
}

BOOST_AUTO_TEST_CASE(Days_HighBitsIgnored)
{
	// Only the low 7 bits (Mon..Sun) are meaningful; a stray high bit must not
	// change the classification of an otherwise-expressible selection.
	BOOST_CHECK(ClassifyDaySelection(0xff) == ControllerDaySelection::AllDays);
	BOOST_CHECK(ClassifyDaySelection(0x80 | 0x01) == ControllerDaySelection::SingleDay);
}

//=============================================================================
// Candidate feasibility.
//=============================================================================

BOOST_AUTO_TEST_CASE(Candidate_ValidAllDays_IsPromotable)
{
	const auto r = CheckControllerCandidate(ValidCandidate());
	BOOST_CHECK(r.promotable);
	BOOST_CHECK(r.blockers.empty());
	BOOST_CHECK(r.day_selection == ControllerDaySelection::AllDays);
}

BOOST_AUTO_TEST_CASE(Candidate_SingleDayAndWeekends_Promotable)
{
	auto single = ValidCandidate();
	single.days_of_week = 0x04; // Wednesday
	auto rs = CheckControllerCandidate(single);
	BOOST_CHECK(rs.promotable);
	BOOST_CHECK(rs.day_selection == ControllerDaySelection::SingleDay);

	auto wknd = ValidCandidate();
	wknd.days_of_week = 0x60;
	auto rw = CheckControllerCandidate(wknd);
	BOOST_CHECK(rw.promotable);
	BOOST_CHECK(rw.day_selection == ControllerDaySelection::Weekends);
}

BOOST_AUTO_TEST_CASE(Candidate_MissingTarget_Blocked)
{
	auto c = ValidCandidate();
	c.target.clear();
	const auto r = CheckControllerCandidate(c);
	BOOST_CHECK(!r.promotable);
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::TargetMissing));
}

BOOST_AUTO_TEST_CASE(Candidate_ArbitraryDays_Blocked)
{
	auto c = ValidCandidate();
	c.days_of_week = 0x15; // Mon+Wed+Fri — the classic "controller can't do this"
	const auto r = CheckControllerCandidate(c);
	BOOST_CHECK(!r.promotable);
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::DaySelectionNotExpressible));
	BOOST_CHECK(r.day_selection == ControllerDaySelection::NotExpressible);
}

BOOST_AUTO_TEST_CASE(Candidate_BadTime_Blocked)
{
	auto c = ValidCandidate();
	c.off_hour = 25; // out of range
	const auto r = CheckControllerCandidate(c);
	BOOST_CHECK(!r.promotable);
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::TimeInvalid));
}

BOOST_AUTO_TEST_CASE(Candidate_MultipleBlockers_AllReported)
{
	ControllerSchedule c;              // empty target
	c.days_of_week = 0x15;             // arbitrary combo
	c.on_hour = 9; c.off_hour = 30;    // bad off hour
	const auto r = CheckControllerCandidate(c);
	BOOST_CHECK(!r.promotable);
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::TargetMissing));
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::DaySelectionNotExpressible));
	BOOST_CHECK(HasBlocker(r, PromotionBlocker::TimeInvalid));
}

//=============================================================================
// Action-kind gate — which app actions can ever be a controller program.
//=============================================================================

BOOST_AUTO_TEST_CASE(Action_OnlyOnOffRepresentable)
{
	BOOST_CHECK(IsControllerRepresentableAction(ActionType::ButtonOn));
	BOOST_CHECK(IsControllerRepresentableAction(ActionType::ButtonOff));

	BOOST_CHECK(!IsControllerRepresentableAction(ActionType::ButtonToggle));
	BOOST_CHECK(!IsControllerRepresentableAction(ActionType::PoolSetpoint));
	BOOST_CHECK(!IsControllerRepresentableAction(ActionType::SpaSetpoint));
	BOOST_CHECK(!IsControllerRepresentableAction(ActionType::ChlorinatorPercent));
	BOOST_CHECK(!IsControllerRepresentableAction(ActionType::CirculationMode));
}

//=============================================================================
// Stable code strings (consumed by the UI to localise).
//=============================================================================

//=============================================================================
// Promotion pairing — form a controller span from an app on/off pair.
//=============================================================================

namespace
{
	Schedule AppSchedule(ActionType type, const std::string& target, std::uint8_t days, int hour, int minute)
	{
		Schedule s;
		s.days_of_week = days;
		s.hour = hour;
		s.minute = minute;
		s.action.type = type;
		s.action.target = target;
		return s;
	}
}

BOOST_AUTO_TEST_CASE(Promote_ValidOnOffPair_BuildsSpan)
{
	const auto on = AppSchedule(ActionType::ButtonOn, "Filter Pump", 0x7f, 9, 0);
	const auto off = AppSchedule(ActionType::ButtonOff, "Filter Pump", 0x7f, 17, 30);

	std::string error;
	const auto candidate = BuildPromotionCandidate(on, off, error);
	BOOST_REQUIRE_MESSAGE(candidate.has_value(), error);
	BOOST_CHECK_EQUAL(candidate->target, "Filter Pump");
	BOOST_CHECK_EQUAL(candidate->days_of_week, 0x7f);
	BOOST_CHECK_EQUAL(candidate->on_hour, 9);
	BOOST_CHECK_EQUAL(candidate->on_minute, 0);
	BOOST_CHECK_EQUAL(candidate->off_hour, 17);
	BOOST_CHECK_EQUAL(candidate->off_minute, 30);

	// And the span is representable on the controller.
	BOOST_CHECK(CheckControllerCandidate(*candidate).promotable);
}

BOOST_AUTO_TEST_CASE(Promote_Rejects_BadPairs)
{
	std::string error;
	// On is not a button_on.
	BOOST_CHECK(!BuildPromotionCandidate(
		AppSchedule(ActionType::ButtonToggle, "Filter Pump", 0x7f, 9, 0),
		AppSchedule(ActionType::ButtonOff, "Filter Pump", 0x7f, 17, 0), error).has_value());
	// Off is not a button_off.
	BOOST_CHECK(!BuildPromotionCandidate(
		AppSchedule(ActionType::ButtonOn, "Filter Pump", 0x7f, 9, 0),
		AppSchedule(ActionType::ButtonOn, "Filter Pump", 0x7f, 17, 0), error).has_value());
	// Different targets.
	BOOST_CHECK(!BuildPromotionCandidate(
		AppSchedule(ActionType::ButtonOn, "Filter Pump", 0x7f, 9, 0),
		AppSchedule(ActionType::ButtonOff, "Spa", 0x7f, 17, 0), error).has_value());
	// Different days.
	BOOST_CHECK(!BuildPromotionCandidate(
		AppSchedule(ActionType::ButtonOn, "Filter Pump", 0x1f, 9, 0),
		AppSchedule(ActionType::ButtonOff, "Filter Pump", 0x7f, 17, 0), error).has_value());
}

BOOST_AUTO_TEST_CASE(Strings_Stable)
{
	BOOST_CHECK_EQUAL(ControllerDaySelectionToString(ControllerDaySelection::Weekdays), "weekdays");
	BOOST_CHECK_EQUAL(ControllerDaySelectionToString(ControllerDaySelection::NotExpressible), "not_expressible");
	BOOST_CHECK_EQUAL(PromotionBlockerToString(PromotionBlocker::DaySelectionNotExpressible), "day_selection_not_expressible");
	BOOST_CHECK_EQUAL(PromotionBlockerToString(PromotionBlocker::ActionNotOnOff), "action_not_on_off");
}

BOOST_AUTO_TEST_SUITE_END()
