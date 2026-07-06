#include <optional>
#include <string>

#include <boost/test/unit_test.hpp>

#include "devices/onetouch/onetouch_screen_reader.h"
#include "utility/screen_data_page.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices::OneTouch;

// Pure row-scraping primitives shared by the OneTouch keypad goals (value edit, spa-switch
// picker, schedule write). Extracted from OneTouchDevice into onetouch_screen_reader.h so
// they can be exercised directly against a hand-built ScreenDataPage - no device, no bus.
// These assertions pin the behaviour the device methods had before extraction.

namespace
{
	// Build an N-line screen with the given rows filled in (rest blank). Mirrors how a
	// reconstructed OneTouch page presents to the readers: page[i].Text is the row content.
	Utility::ScreenDataPage MakePage(const std::vector<std::string>& rows)
	{
		Utility::ScreenDataPage page(rows.size());
		for (std::size_t i = 0; i < rows.size(); ++i)
		{
			page[i].Text = rows[i];
		}
		return page;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_OneTouch_ScreenReader)

//=========================================================================
// SanitiseFunctionText
//=========================================================================

BOOST_AUTO_TEST_CASE(SanitiseFunctionText_TrimsEdgesPreservesInterior, * boost::unit_test::label("unit"))
{
	BOOST_TEST(SanitiseFunctionText("Pool Light") == "Pool Light");
	BOOST_TEST(SanitiseFunctionText("   Pool Light   ") == "Pool Light");
	BOOST_TEST(SanitiseFunctionText("Equipment ON/OFF") == "Equipment ON/OFF");
}

BOOST_AUTO_TEST_CASE(SanitiseFunctionText_StripsNonPrintableEdges, * boost::unit_test::label("unit"))
{
	BOOST_TEST(SanitiseFunctionText("\tSpa Jets\r\n") == "Spa Jets");
	// Leading 0x01 and trailing 0x7f are trimmed as non-printable.
	BOOST_TEST(SanitiseFunctionText(std::string("\x01Spillway\x7f", 10)) == "Spillway");
}

BOOST_AUTO_TEST_CASE(SanitiseFunctionText_BlankOrEmptyYieldsEmpty, * boost::unit_test::label("unit"))
{
	BOOST_TEST(SanitiseFunctionText("").empty());
	BOOST_TEST(SanitiseFunctionText("                ").empty());
	BOOST_TEST(SanitiseFunctionText(std::string("\0\0\0", 3)).empty());
}

//=========================================================================
// FirstNumber
//=========================================================================

BOOST_AUTO_TEST_CASE(FirstNumber_ReadsFirstContiguousDigitRun, * boost::unit_test::label("unit"))
{
	BOOST_TEST(FirstNumber("Pool Heat   90`F").value() == 90);
	BOOST_TEST(FirstNumber("Set Pool to:  45%").value() == 45);
	BOOST_TEST(FirstNumber("Spa Heat   102`F").value() == 102);
}

BOOST_AUTO_TEST_CASE(FirstNumber_StopsAtFirstRun, * boost::unit_test::label("unit"))
{
	// Only the FIRST contiguous run is the value; later digits are ignored.
	BOOST_TEST(FirstNumber("12 of 34").value() == 12);
}

BOOST_AUTO_TEST_CASE(FirstNumber_NoDigitsIsNullopt, * boost::unit_test::label("unit"))
{
	BOOST_TEST(FirstNumber("Operate at 100%").value() == 100);  // a digit anywhere -> value
	BOOST_TEST(!FirstNumber("No Programs").has_value());
	BOOST_TEST(!FirstNumber("").has_value());
}

//=========================================================================
// DisplayedValue
//=========================================================================

BOOST_AUTO_TEST_CASE(DisplayedValue_ReadsRow, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "    Set Temp", "", "Pool Heat   90`F", "Spa Heat   102`F" });
	BOOST_TEST(DisplayedValue(page, 2).value() == 90);
	BOOST_TEST(DisplayedValue(page, 3).value() == 102);
}

BOOST_AUTO_TEST_CASE(DisplayedValue_OutOfRangeOrBlankIsNullopt, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "Pool Heat   90`F", "" });
	BOOST_TEST(!DisplayedValue(page, 1).has_value());   // blank row
	BOOST_TEST(!DisplayedValue(page, 9).has_value());   // out of range
}

//=========================================================================
// DisplayedFunctionOnRow
//=========================================================================

BOOST_AUTO_TEST_CASE(DisplayedFunctionOnRow_TrimsRowText, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "   Spa Switch", "", "1:2   Pool Light" });
	BOOST_TEST(DisplayedFunctionOnRow(page, 2).value() == "1:2   Pool Light");
}

BOOST_AUTO_TEST_CASE(DisplayedFunctionOnRow_BlankOrOutOfRangeIsNullopt, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "Text", "     " });
	BOOST_TEST(!DisplayedFunctionOnRow(page, 1).has_value());   // blank row
	BOOST_TEST(!DisplayedFunctionOnRow(page, 9).has_value());   // out of range
}

//=========================================================================
// FindLineStartingWith
//=========================================================================

BOOST_AUTO_TEST_CASE(FindLineStartingWith_CaseInsensitivePrefix, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "  System Setup", " Spa Switch", " Freeze Protect" });
	BOOST_TEST(FindLineStartingWith(page, "Spa Switch").value() == 1);
	BOOST_TEST(FindLineStartingWith(page, "spa switch").value() == 1);   // case-insensitive
	BOOST_TEST(FindLineStartingWith(page, "System").value() == 0);       // ignores leading padding
}

BOOST_AUTO_TEST_CASE(FindLineStartingWith_FirstMatchWins, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "Aux1  OFF", "Aux2  OFF", "Aux1  ON" });
	BOOST_TEST(FindLineStartingWith(page, "Aux1").value() == 0);
}

BOOST_AUTO_TEST_CASE(FindLineStartingWith_NotFoundIsNullopt, * boost::unit_test::label("unit"))
{
	const auto page = MakePage({ "Aux1  OFF", "Aux2  OFF" });
	BOOST_TEST(!FindLineStartingWith(page, "Pool Heat").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
