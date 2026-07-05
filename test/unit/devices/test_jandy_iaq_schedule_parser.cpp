#include <boost/test/unit_test.hpp>

#include "devices/iaq/iaq_schedule_parser.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices::IAQ;

// Schedule-list rows are TAB-separated on the wire; the message layer sanitises the
// non-printable TAB (0x09) to '?', so the string ParseScheduleRow actually receives is
// '?'-delimited. The parser accepts either; tests use '\t' for readability plus one
// case in the exact production '?' form.

BOOST_AUTO_TEST_SUITE(TestSuite_IAQ_ScheduleParser)

//=============================================================================
// Day-token mapping (bit0 = Monday .. bit6 = Sunday). The IAQ UI only ever
// offers all-days / weekdays / weekends / a single day.
//=============================================================================

BOOST_AUTO_TEST_CASE(DayToken_AllVariants)
{
	BOOST_CHECK_EQUAL(ParseDayToken("All").value(),    DayMask::AllDays);
	BOOST_CHECK_EQUAL(ParseDayToken("Wkdays").value(), DayMask::Weekdays);
	BOOST_CHECK_EQUAL(ParseDayToken("Wkends").value(), DayMask::Weekends);
	BOOST_CHECK_EQUAL(ParseDayToken("Su").value(), DayMask::Sunday);
	BOOST_CHECK_EQUAL(ParseDayToken("M").value(),  DayMask::Monday);
	BOOST_CHECK_EQUAL(ParseDayToken("Tu").value(), DayMask::Tuesday);
	BOOST_CHECK_EQUAL(ParseDayToken("W").value(),  DayMask::Wednesday);
	BOOST_CHECK_EQUAL(ParseDayToken("Th").value(), DayMask::Thursday);
	BOOST_CHECK_EQUAL(ParseDayToken("F").value(),  DayMask::Friday);
	BOOST_CHECK_EQUAL(ParseDayToken("Sa").value(), DayMask::Saturday);

	BOOST_CHECK_EQUAL(DayMask::Weekdays, 0x1f);
	BOOST_CHECK_EQUAL(DayMask::Weekends, 0x60);
	BOOST_CHECK_EQUAL(DayMask::AllDays,  0x7f);
}

BOOST_AUTO_TEST_CASE(DayToken_Unknown_ReturnsNullopt)
{
	BOOST_CHECK(!ParseDayToken("Mon").has_value());
	BOOST_CHECK(!ParseDayToken("").has_value());
	BOOST_CHECK(!ParseDayToken("Everyday").has_value());
}

//=============================================================================
// Real rows captured from a live iAQ page 0x28 (Schedule Group A).
//=============================================================================

BOOST_AUTO_TEST_CASE(Row_FilterPump_AllDays_Captured)
{
	auto s = ParseScheduleRow("Filter Pump\t11:00 AM\t2:00 PM\tAll");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Filter Pump");
	BOOST_CHECK_EQUAL(s->on_hour, 11);
	BOOST_CHECK_EQUAL(s->on_minute, 0);
	BOOST_CHECK_EQUAL(s->off_hour, 14);   // 2:00 PM
	BOOST_CHECK_EQUAL(s->off_minute, 0);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::AllDays);
	BOOST_CHECK(s->enabled);              // controller offers delete only
}

BOOST_AUTO_TEST_CASE(Row_ProductionQuestionMarkSeparators)
{
	// Exactly what msg.Line() yields once the wire TABs are sanitised to '?'.
	auto s = ParseScheduleRow("Filter Pump?11:00 AM?2:00 PM?All");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Filter Pump");
	BOOST_CHECK_EQUAL(s->on_hour, 11);
	BOOST_CHECK_EQUAL(s->off_hour, 14);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::AllDays);
}

BOOST_AUTO_TEST_CASE(Row_MultiWordTarget_SolarHeat_Overnight)
{
	// Off (11:00 AM) earlier than On (2:00 PM) => overnight span, preserved verbatim.
	auto s = ParseScheduleRow("Solar Heat\t2:00 PM\t11:00 AM\tAll");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Solar Heat");
	BOOST_CHECK_EQUAL(s->on_hour, 14);
	BOOST_CHECK_EQUAL(s->off_hour, 11);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::AllDays);
}

BOOST_AUTO_TEST_CASE(Row_Spillway_And_PoolHeat)
{
	auto spill = ParseScheduleRow("Spillway\t11:00 AM\t2:00 PM\tAll");
	BOOST_REQUIRE(spill.has_value());
	BOOST_CHECK_EQUAL(spill->target, "Spillway");

	auto heat = ParseScheduleRow("Pool Heat\t11:00 AM\t2:00 PM\tAll");
	BOOST_REQUIRE(heat.has_value());
	BOOST_CHECK_EQUAL(heat->target, "Pool Heat");
	BOOST_CHECK_EQUAL(heat->on_hour, 11);
	BOOST_CHECK_EQUAL(heat->off_hour, 14);
}

//=============================================================================
// The schedules the user created during the capture, in single-day form.
//=============================================================================

BOOST_AUTO_TEST_CASE(Row_CreatedA_FilterPump_Monday)
{
	auto s = ParseScheduleRow("Filter Pump\t9:00 AM\t5:00 PM\tM");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Filter Pump");
	BOOST_CHECK_EQUAL(s->on_hour, 9);
	BOOST_CHECK_EQUAL(s->on_minute, 0);
	BOOST_CHECK_EQUAL(s->off_hour, 17);   // 5:00 PM
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Monday);
}

BOOST_AUTO_TEST_CASE(Row_CreatedB_PoolLight_Wednesday)
{
	auto s = ParseScheduleRow("Pool Light\t6:30 AM\t8:15 AM\tW");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Pool Light");
	BOOST_CHECK_EQUAL(s->on_hour, 6);
	BOOST_CHECK_EQUAL(s->on_minute, 30);
	BOOST_CHECK_EQUAL(s->off_hour, 8);
	BOOST_CHECK_EQUAL(s->off_minute, 15);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Wednesday);
}

//=============================================================================
// 12-hour <-> 24-hour boundary conversions.
//=============================================================================

BOOST_AUTO_TEST_CASE(Time_MidnightAndNoon_Boundaries)
{
	auto s = ParseScheduleRow("Spa\t12:00 AM\t12:00 PM\tWkdays");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->on_hour, 0);     // 12:00 AM -> 00:00
	BOOST_CHECK_EQUAL(s->off_hour, 12);   // 12:00 PM -> 12:00
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Weekdays);
}

BOOST_AUTO_TEST_CASE(Time_LateEvening_Weekends)
{
	auto s = ParseScheduleRow("Spa Jets\t10:45 PM\t6:05 AM\tWkends");
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Spa Jets");
	BOOST_CHECK_EQUAL(s->on_hour, 22);
	BOOST_CHECK_EQUAL(s->on_minute, 45);
	BOOST_CHECK_EQUAL(s->off_hour, 6);
	BOOST_CHECK_EQUAL(s->off_minute, 5);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Weekends);
}

//=============================================================================
// Malformed rows must be rejected (nullopt), never a half-parsed schedule.
//=============================================================================

BOOST_AUTO_TEST_CASE(Row_Malformed_ReturnsNullopt)
{
	BOOST_CHECK(!ParseScheduleRow("").has_value());
	BOOST_CHECK(!ParseScheduleRow("Filter Pump").has_value());                       // 1 field
	BOOST_CHECK(!ParseScheduleRow("Filter Pump\t11:00 AM\t2:00 PM").has_value());    // no day (3 fields)
	BOOST_CHECK(!ParseScheduleRow("\t11:00 AM\t2:00 PM\tAll").has_value());          // no target
	BOOST_CHECK(!ParseScheduleRow("Pump\t25:00 AM\t2:00 PM\tAll").has_value());      // bad hour
	BOOST_CHECK(!ParseScheduleRow("Pump\t11:00 XM\t2:00 PM\tAll").has_value());      // bad meridiem
	BOOST_CHECK(!ParseScheduleRow("Pump\t1100 AM\t2:00 PM\tAll").has_value());       // no colon
	BOOST_CHECK(!ParseScheduleRow("Pump\t11:00 AM\t2:00 PM\tSomeday").has_value());  // bad day token
	BOOST_CHECK(!ParseScheduleRow("A\tB\t11:00 AM\t2:00 PM\tAll").has_value());      // 5 fields
}

BOOST_AUTO_TEST_SUITE_END()
