#include "devices/iaq/iaq_schedule_parser.h"

#include <array>
#include <charconv>
#include <vector>

namespace AqualinkAutomate::Devices::IAQ
{

	namespace
	{

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
			while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) { s.remove_suffix(1); }
			return s;
		}

		// Whitespace-split into non-empty tokens.
		std::vector<std::string_view> Tokenise(std::string_view text)
		{
			std::vector<std::string_view> tokens;
			std::size_t i = 0;
			while (i < text.size())
			{
				while (i < text.size() && text[i] == ' ') { ++i; }
				const std::size_t start = i;
				while (i < text.size() && text[i] != ' ') { ++i; }
				if (i > start) { tokens.emplace_back(text.substr(start, i - start)); }
			}
			return tokens;
		}

		// Split into fields on the row separator. On the wire the IAQ TableMessage
		// separates schedule fields with a TAB (0x09); the message layer sanitises that
		// non-printable byte to '?', so the string the parser actually receives is
		// '?'-delimited. Accept either. Spaces are NOT separators -- they occur inside a
		// field ("11:00 AM"). Trailing empty fields (from the row's NUL terminator) drop.
		std::vector<std::string_view> SplitFields(std::string_view text)
		{
			std::vector<std::string_view> fields;
			std::size_t start = 0;
			for (std::size_t i = 0; i <= text.size(); ++i)
			{
				if (i == text.size() || text[i] == '\t' || text[i] == '?')
				{
					if (const auto field = Trim(text.substr(start, i - start)); !field.empty()) { fields.push_back(field); }
					start = i + 1;
				}
			}
			return fields;
		}

		// Parse an "h:mm" 12-hour clock token plus a separate "AM"/"PM" marker into a
		// 24-hour (hour, minute). Returns false on any malformed field.
		bool Parse12HourTime(std::string_view hhmm, std::string_view meridiem, int& out_hour, int& out_minute)
		{
			const auto colon = hhmm.find(':');
			if (colon == std::string_view::npos) { return false; }

			int hour = 0;
			int minute = 0;

			const auto h = hhmm.substr(0, colon);
			const auto m = hhmm.substr(colon + 1);
			if (h.empty() || m.empty()) { return false; }

			if (std::from_chars(h.data(), h.data() + h.size(), hour).ec != std::errc{}) { return false; }
			if (std::from_chars(m.data(), m.data() + m.size(), minute).ec != std::errc{}) { return false; }
			if (hour < 1 || hour > 12 || minute < 0 || minute > 59) { return false; }

			bool is_pm = false;
			if (meridiem == "PM" || meridiem == "pm") { is_pm = true; }
			else if (meridiem == "AM" || meridiem == "am") { is_pm = false; }
			else { return false; }

			// 12 AM -> 00:xx, 12 PM -> 12:xx, otherwise add 12 for PM.
			if (is_pm) { hour = (hour == 12) ? 12 : hour + 12; }
			else       { hour = (hour == 12) ? 0  : hour; }

			out_hour = hour;
			out_minute = minute;
			return true;
		}

	}
	// unnamed namespace

	std::optional<std::uint8_t> ParseDayToken(std::string_view token)
	{
		if (token == "All")    { return DayMask::AllDays; }
		if (token == "Wkdays") { return DayMask::Weekdays; }
		if (token == "Wkends") { return DayMask::Weekends; }
		if (token == "Su")     { return DayMask::Sunday; }
		if (token == "M")      { return DayMask::Monday; }
		if (token == "Tu")     { return DayMask::Tuesday; }
		if (token == "W")      { return DayMask::Wednesday; }
		if (token == "Th")     { return DayMask::Thursday; }
		if (token == "F")      { return DayMask::Friday; }
		if (token == "Sa")     { return DayMask::Saturday; }
		return std::nullopt;
	}

	namespace
	{
		// Parse a single "<h:mm> <AM|PM>" field into 24-hour (hour, minute).
		bool ParseTimeField(std::string_view field, int& out_hour, int& out_minute)
		{
			const auto parts = Tokenise(field);   // e.g. "11:00 AM" -> {"11:00","AM"}
			if (parts.size() != 2) { return false; }
			return Parse12HourTime(parts[0], parts[1], out_hour, out_minute);
		}
	}

	std::optional<Scheduling::ControllerSchedule> ParseScheduleRow(std::string_view row)
	{
		// Row shape (tab/'?'-separated): <Target> | <On h:mm AM/PM> | <Off h:mm AM/PM> | <Days>.
		const auto fields = SplitFields(row);
		if (fields.size() != 4) { return std::nullopt; }

		Scheduling::ControllerSchedule schedule;

		const std::string_view target = fields[0];
		if (target.empty()) { return std::nullopt; }

		if (!ParseTimeField(fields[1], schedule.on_hour, schedule.on_minute)) { return std::nullopt; }
		if (!ParseTimeField(fields[2], schedule.off_hour, schedule.off_minute)) { return std::nullopt; }

		const auto days = ParseDayToken(fields[3]);
		if (!days.has_value()) { return std::nullopt; }
		schedule.days_of_week = days.value();

		schedule.target = std::string(target);
		schedule.enabled = true; // the controller offers delete only, never disable
		return schedule;
	}

}
// namespace AqualinkAutomate::Devices::IAQ
