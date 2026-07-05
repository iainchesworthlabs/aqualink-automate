#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "scheduling/controller_schedule.h"
#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Devices::OneTouch
{

	// Day-of-week bitmask used by Scheduling::ControllerSchedule: bit0 = Monday ..
	// bit6 = Sunday. The OneTouch Program UI, like the IAQ, only permits a restricted
	// set of day selections (every day, weekdays, weekends, or exactly one day), so
	// only those masks are ever produced here. (Deliberately kept independent of the
	// IAQ DayMask: the on-screen tokens differ - "All Days"/"Weekdays"/"Mon" here vs
	// "All"/"Wkdays"/"M" on the IAQ wire - so the two parsers do not share a token map.)
	namespace DayMask
	{
		inline constexpr std::uint8_t Monday    = 0x01;
		inline constexpr std::uint8_t Tuesday   = 0x02;
		inline constexpr std::uint8_t Wednesday = 0x04;
		inline constexpr std::uint8_t Thursday  = 0x08;
		inline constexpr std::uint8_t Friday    = 0x10;
		inline constexpr std::uint8_t Saturday  = 0x20;
		inline constexpr std::uint8_t Sunday    = 0x40;
		inline constexpr std::uint8_t Weekdays  = Monday | Tuesday | Wednesday | Thursday | Friday; // 0x1f
		inline constexpr std::uint8_t Weekends  = Saturday | Sunday;                                // 0x60
		inline constexpr std::uint8_t AllDays   = Weekdays | Weekends;                              // 0x7f
	}

	// Map a OneTouch Program-detail days row ("All Days", "Weekdays", "Weekends", or a
	// single day "Mon"/"Tue"/"Wed"/"Thu"/"Fri"/"Sat"/"Sun") to a DayMask value. The row
	// text is trimmed and matched case-insensitively. Returns nullopt for an
	// unrecognised token (e.g. a blank row or a non-days line).
	std::optional<std::uint8_t> ParseDaysRow(std::string_view row);

	// Parse the OneTouch per-equipment Program DETAIL page (a 16x12 reconstructed text
	// screen) into a ControllerSchedule. The page layout is:
	//
	//     line 0: "<Equipment Name>"    (the schedule's target)
	//     line 2: "Pgm N of M"          (this program's index / total for the equipment)
	//     line 3: "ON   <h:mm> <AM|PM>"
	//     line 4: "OFF  <h:mm> <AM|PM>"
	//     line 5: "<Days>"              (All Days / Weekdays / Weekends / a single day)
	//     lines 9-11: Add/Delete/Change Program action rows
	//
	// Times are 12-hour + AM/PM on screen and converted to the struct's 24-hour fields;
	// an off-time earlier than the on-time denotes an overnight span and is preserved
	// verbatim. Only target/days/on/off are populated (the caller assigns id/name/group).
	// The optional out_program_index / out_program_count receive the parsed "Pgm N of M".
	//
	// Returns nullopt when the page is NOT a program-detail page (e.g. an equipment with
	// "No Programs", or a malformed / mis-detected page): specifically when the ON row, the
	// OFF row, or the days row cannot be parsed, or the target line is empty.
	std::optional<Scheduling::ControllerSchedule> ParseProgramDetailPage(
		const Utility::ScreenDataPage& page,
		int* out_program_index = nullptr,
		int* out_program_count = nullptr);

	// Overload operating on the raw screen lines (index-aligned, at least 6 entries) so
	// the parser can be unit-tested without constructing a ScreenDataPage. Line indices
	// match the on-screen layout above.
	std::optional<Scheduling::ControllerSchedule> ParseProgramDetailLines(
		const std::vector<std::string>& lines,
		int* out_program_index = nullptr,
		int* out_program_count = nullptr);

}
// namespace AqualinkAutomate::Devices::OneTouch
