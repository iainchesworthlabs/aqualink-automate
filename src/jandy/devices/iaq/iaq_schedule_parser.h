#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Devices::IAQ
{

	// Day-of-week bitmask used by Scheduling::ControllerSchedule: bit0 = Monday ..
	// bit6 = Sunday. The IAQ / AqualinkTouch schedule UI only permits a restricted
	// set of day selections (every day, weekdays, weekends, or exactly one day), so
	// only those masks are ever produced or accepted here.
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

	// Map a wire day token from the schedule row ("All", "Wkdays", "Wkends", or a
	// single-day abbreviation "Su"/"M"/"Tu"/"W"/"Th"/"F"/"Sa") to a DayMask value.
	// Returns nullopt for an unrecognised token.
	std::optional<std::uint8_t> ParseDayToken(std::string_view token);

	// Parse one Schedule-list row as displayed on IAQ page 0x28 (the ASCII text of
	// a TableMessage 0x26 entry) into a ControllerSchedule. The row shape is:
	//
	//     <Target> <On h:mm> <AM|PM> <Off h:mm> <AM|PM> <Days>
	//
	// where <Target> is an equipment label that may itself contain spaces (parsed
	// as whatever remains to the left once the five trailing fields are removed).
	// Times are 12-hour on the wire and converted to the struct's 24-hour fields; an
	// off-time earlier than the on-time denotes an overnight span and is preserved
	// verbatim. Only target/days/on/off are populated; the caller assigns id/name.
	// Returns nullopt if the row does not match the expected shape.
	std::optional<Scheduling::ControllerSchedule> ParseScheduleRow(std::string_view row);

}
// namespace AqualinkAutomate::Devices::IAQ
