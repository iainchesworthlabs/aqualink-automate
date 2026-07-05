#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "devices/onetouch/onetouch_schedule_parser.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices::OneTouch;

// The OneTouch renders a 16x12 character screen. The per-equipment Program DETAIL page
// (reconstructed by the Screen capability) is parsed by ParseProgramDetailLines. The
// on-screen rows are right/centre-justified with generous whitespace; the parser trims
// and tokenises, so the exact column padding is irrelevant. Line indices matter:
//   0 = target, 2 = "Pgm N of M", 3 = ON, 4 = OFF, 5 = days.

namespace
{
	// Build the canonical 12-line detail page with the given rows filled in and the rest
	// blank, mirroring the live capture layout (see docs/onetouch_schedule_protocol.md).
	std::vector<std::string> MakeDetailPage(
		const std::string& target,
		const std::string& pgm,
		const std::string& on_row,
		const std::string& off_row,
		const std::string& days_row)
	{
		return {
			target,             // 0
			"                ", // 1
			pgm,                // 2
			on_row,             // 3
			off_row,            // 4
			days_row,           // 5
			"                ", // 6
			"                ", // 7
			"                ", // 8
			" Add      Program", // 9
			" Delete   Program", // 10
			" Change   Program"  // 11
		};
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_OneTouch_ScheduleParser)

//=============================================================================
// Days-row mapping (bit0 = Monday .. bit6 = Sunday). The OneTouch Program UI
// offers all-days / weekdays / weekends / a single day, tokenised on-screen as
// "All Days" / "Weekdays" / "Weekends" / "Mon".."Sun".
//=============================================================================

BOOST_AUTO_TEST_CASE(DaysRow_AllVariants)
{
	BOOST_CHECK_EQUAL(ParseDaysRow(" All Days         ").value(), DayMask::AllDays);
	BOOST_CHECK_EQUAL(ParseDaysRow("Weekdays").value(),           DayMask::Weekdays);
	BOOST_CHECK_EQUAL(ParseDaysRow("Weekends").value(),           DayMask::Weekends);
	BOOST_CHECK_EQUAL(ParseDaysRow(" Mon ").value(), DayMask::Monday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Tue").value(),   DayMask::Tuesday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Wed").value(),   DayMask::Wednesday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Thu").value(),   DayMask::Thursday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Fri").value(),   DayMask::Friday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Sat").value(),   DayMask::Saturday);
	BOOST_CHECK_EQUAL(ParseDaysRow("Sun").value(),   DayMask::Sunday);

	BOOST_CHECK_EQUAL(DayMask::Weekdays, 0x1f);
	BOOST_CHECK_EQUAL(DayMask::Weekends, 0x60);
	BOOST_CHECK_EQUAL(DayMask::AllDays,  0x7f);
}

BOOST_AUTO_TEST_CASE(DaysRow_Unknown_ReturnsNullopt)
{
	BOOST_CHECK(!ParseDaysRow("").has_value());
	BOOST_CHECK(!ParseDaysRow("   ").has_value());
	BOOST_CHECK(!ParseDaysRow("M").has_value());        // IAQ token, not the OneTouch's
	BOOST_CHECK(!ParseDaysRow("Everyday").has_value());
}

//=============================================================================
// The exact captured page: Filter Pump, ON 11:00 AM -> OFF 2:00 PM, All Days.
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_FilterPump_AllDays_Captured)
{
	const auto page = MakeDetailPage(
		"   Filter Pump    ",
		"    Pgm 1 of 1    ",
		" ON      11:00 AM ",
		" OFF      2:00 PM ",
		" All Days         ");

	int index = -1;
	int count = -1;
	auto s = ParseProgramDetailLines(page, &index, &count);
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Filter Pump");
	BOOST_CHECK_EQUAL(s->on_hour, 11);
	BOOST_CHECK_EQUAL(s->on_minute, 0);
	BOOST_CHECK_EQUAL(s->off_hour, 14);   // 2:00 PM
	BOOST_CHECK_EQUAL(s->off_minute, 0);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::AllDays);
	BOOST_CHECK(s->enabled);              // controller offers delete only
	BOOST_CHECK_EQUAL(index, 1);
	BOOST_CHECK_EQUAL(count, 1);
}

//=============================================================================
// A single-day variant and a weekdays variant.
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_SpaHeat_SingleDay_Wednesday)
{
	const auto page = MakeDetailPage(
		"    Spa Heat      ",
		"    Pgm 2 of 3    ",
		" ON       6:30 AM ",
		" OFF      8:15 AM ",
		" Wed              ");

	int index = -1;
	int count = -1;
	auto s = ParseProgramDetailLines(page, &index, &count);
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Spa Heat");   // multi-word target preserved
	BOOST_CHECK_EQUAL(s->on_hour, 6);
	BOOST_CHECK_EQUAL(s->on_minute, 30);
	BOOST_CHECK_EQUAL(s->off_hour, 8);
	BOOST_CHECK_EQUAL(s->off_minute, 15);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Wednesday);
	BOOST_CHECK_EQUAL(index, 2);
	BOOST_CHECK_EQUAL(count, 3);
}

BOOST_AUTO_TEST_CASE(DetailPage_PoolLight_Weekdays_Overnight)
{
	// Off (6:00 AM) earlier than On (10:45 PM) => overnight span, preserved verbatim.
	const auto page = MakeDetailPage(
		"   Pool Light     ",
		"    Pgm 1 of 1    ",
		" ON      10:45 PM ",
		" OFF      6:00 AM ",
		" Weekdays         ");

	auto s = ParseProgramDetailLines(page);
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Pool Light");
	BOOST_CHECK_EQUAL(s->on_hour, 22);   // 10:45 PM
	BOOST_CHECK_EQUAL(s->on_minute, 45);
	BOOST_CHECK_EQUAL(s->off_hour, 6);
	BOOST_CHECK_EQUAL(s->off_minute, 0);
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Weekdays);
}

//=============================================================================
// 12-hour <-> 24-hour boundary conversions (midnight / noon).
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_MidnightAndNoon_Boundaries)
{
	const auto page = MakeDetailPage(
		"       Spa        ",
		"    Pgm 1 of 1    ",
		" ON      12:00 AM ",
		" OFF     12:00 PM ",
		" Weekends         ");

	auto s = ParseProgramDetailLines(page);
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->on_hour, 0);     // 12:00 AM -> 00:00
	BOOST_CHECK_EQUAL(s->off_hour, 12);   // 12:00 PM -> 12:00
	BOOST_CHECK_EQUAL(s->days_of_week, DayMask::Weekends);
}

//=============================================================================
// "No Programs" placeholder => nullopt (this equipment has no schedule).
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_NoPrograms_ReturnsNullopt)
{
	// An equipment with no program shows "No Programs" + "Add Program"/"New Program".
	const std::vector<std::string> page = {
		"   Filter Pump    ", // 0
		"                  ", // 1
		"   No Programs    ", // 2  (no "Pgm N of M")
		"                  ", // 3  (no ON row)
		"                  ", // 4  (no OFF row)
		"                  ", // 5  (no days row)
		"                  ", // 6
		"                  ", // 7
		"                  ", // 8
		" Add      Program ", // 9
		" New      Program ", // 10
		"                  "  // 11
	};

	BOOST_CHECK(!ParseProgramDetailLines(page).has_value());

	// An explicit "No Programs" on the target line is also rejected.
	const auto explicit_none = MakeDetailPage(
		"   No Programs    ",
		"                  ",
		"                  ",
		"                  ",
		"                  ");
	BOOST_CHECK(!ParseProgramDetailLines(explicit_none).has_value());
}

//=============================================================================
// Malformed pages must be rejected (nullopt), never a half-parsed schedule.
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_Malformed_ReturnsNullopt)
{
	// Too few lines.
	BOOST_CHECK(!ParseProgramDetailLines({ "Filter Pump", "", "Pgm 1 of 1" }).has_value());

	// Empty target line.
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		"                  ", "  Pgm 1 of 1  ", " ON  11:00 AM ", " OFF  2:00 PM ", " All Days ")).has_value());

	// Bad ON hour (25).
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		" Filter Pump ", " Pgm 1 of 1 ", " ON  25:00 AM ", " OFF  2:00 PM ", " All Days ")).has_value());

	// Bad meridiem.
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		" Filter Pump ", " Pgm 1 of 1 ", " ON  11:00 XM ", " OFF  2:00 PM ", " All Days ")).has_value());

	// No colon in the time.
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		" Filter Pump ", " Pgm 1 of 1 ", " ON  1100 AM ", " OFF  2:00 PM ", " All Days ")).has_value());

	// Missing OFF row (wrong prefix on line 4).
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		" Filter Pump ", " Pgm 1 of 1 ", " ON  11:00 AM ", " END  2:00 PM ", " All Days ")).has_value());

	// Unrecognised days token.
	BOOST_CHECK(!ParseProgramDetailLines(MakeDetailPage(
		" Filter Pump ", " Pgm 1 of 1 ", " ON  11:00 AM ", " OFF  2:00 PM ", " Someday ")).has_value());
}

//=============================================================================
// A missing/garbled "Pgm N of M" row does NOT invalidate the page: the ON/OFF/days
// triple is the ground truth. Index/count simply default to 1-of-1.
//=============================================================================

BOOST_AUTO_TEST_CASE(DetailPage_MissingPgmRow_DefaultsToOneOfOne)
{
	const auto page = MakeDetailPage(
		"   Filter Pump    ",
		"                  ",   // Pgm row blank (mid-transition render)
		" ON      11:00 AM ",
		" OFF      2:00 PM ",
		" All Days         ");

	int index = -1;
	int count = -1;
	auto s = ParseProgramDetailLines(page, &index, &count);
	BOOST_REQUIRE(s.has_value());
	BOOST_CHECK_EQUAL(s->target, "Filter Pump");
	BOOST_CHECK_EQUAL(index, 1);
	BOOST_CHECK_EQUAL(count, 1);
}

BOOST_AUTO_TEST_SUITE_END()
