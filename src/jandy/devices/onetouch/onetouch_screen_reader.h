#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Devices::OneTouch
{

	// Pure, device-free reads over a reconstructed OneTouch screen (a ScreenDataPage).
	//
	// These are the row-scraping primitives the on-demand keypad goals share (value edit,
	// spa-switch picker, schedule write) plus the setpoint scrape. Extracted from
	// OneTouchDevice so they can be unit-tested against a hand-built page - no device, no
	// bus - and reused without duplicating the ad-hoc trim / digit-run / case-insensitive
	// prefix logic that previously lived as private lambdas across the state machines.
	//
	// Every function is a pure function of its arguments; none touch device or DataHub state.

	// Trim surrounding whitespace and non-printable bytes (<0x20, 0x7f, space), yielding the
	// clean displayed text. The controller's inverse-video highlight arrives as separate
	// Highlight messages (never appended to the row Text), so a plain trim is exact.
	std::string SanitiseFunctionText(std::string_view raw);

	// The first contiguous run of decimal digits in 'text' as an integer, e.g.
	// "Pool Heat   90`F" -> 90, "Set Pool to: 45%" -> 45. nullopt when the text carries no
	// digit (row not rendered yet / value blanked mid-edit). This is the single decode shared
	// by the row-value read below and the Set AquaPure percentage scrape.
	std::optional<int> FirstNumber(std::string_view text);

	// The integer value shown on a row, exactly as displayed (see FirstNumber). nullopt when
	// line_id is out of range or the row has no parseable value yet.
	std::optional<int> DisplayedValue(const Utility::ScreenDataPage& page, uint8_t line_id);

	// The sanitised function/label text on a row (via SanitiseFunctionText), e.g. "Pool Light"
	// from a Button-Setup row. nullopt when line_id is out of range or the row is blank.
	std::optional<std::string> DisplayedFunctionOnRow(const Utility::ScreenDataPage& page, uint8_t line_id);

	// The first screen line whose sanitised text STARTS WITH 'prefix' (case-insensitive); used
	// to locate the "Spa Switch" menu item and the "S:B" assignment row. nullopt when no line
	// matches.
	std::optional<uint8_t> FindLineStartingWith(const Utility::ScreenDataPage& page, std::string_view prefix);

}
// namespace AqualinkAutomate::Devices::OneTouch
