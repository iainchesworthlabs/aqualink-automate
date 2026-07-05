#include "devices/onetouch/onetouch_schedule_parser.h"

#include <array>
#include <cctype>
#include <charconv>

namespace AqualinkAutomate::Devices::OneTouch
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

		bool IEquals(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size()) { return false; }
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) { return false; }
			}
			return true;
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

			bool is_pm;
			if (IEquals(meridiem, "PM"))      { is_pm = true; }
			else if (IEquals(meridiem, "AM")) { is_pm = false; }
			else                              { return false; }

			// 12 AM -> 00:xx, 12 PM -> 12:xx, otherwise add 12 for PM.
			if (is_pm) { hour = (hour == 12) ? 12 : hour + 12; }
			else       { hour = (hour == 12) ? 0  : hour; }

			out_hour = hour;
			out_minute = minute;
			return true;
		}

		// Parse a time row of the form "<Prefix> <h:mm> <AM|PM>" (Prefix = "ON"/"OFF").
		// The prefix token must match `expected_prefix` (case-insensitive); the trailing
		// two tokens are the time and meridiem. Returns false if the shape does not match.
		bool ParseTimeRow(std::string_view row, std::string_view expected_prefix, int& out_hour, int& out_minute)
		{
			const auto tokens = Tokenise(row);
			if (tokens.size() != 3) { return false; }
			if (!IEquals(tokens[0], expected_prefix)) { return false; }
			return Parse12HourTime(tokens[1], tokens[2], out_hour, out_minute);
		}

		// Parse a "Pgm N of M" row. Returns false if the row does not match that shape.
		bool ParseProgramIndexRow(std::string_view row, int& out_index, int& out_count)
		{
			const auto tokens = Tokenise(row);   // {"Pgm","N","of","M"}
			if (tokens.size() != 4) { return false; }
			if (!IEquals(tokens[0], "Pgm")) { return false; }
			if (!IEquals(tokens[2], "of"))  { return false; }

			int index = 0;
			int count = 0;
			if (std::from_chars(tokens[1].data(), tokens[1].data() + tokens[1].size(), index).ec != std::errc{}) { return false; }
			if (std::from_chars(tokens[3].data(), tokens[3].data() + tokens[3].size(), count).ec != std::errc{}) { return false; }

			out_index = index;
			out_count = count;
			return true;
		}

	}
	// unnamed namespace

	std::optional<std::uint8_t> ParseDaysRow(std::string_view row)
	{
		const auto token = Trim(row);

		if (IEquals(token, "All Days")) { return DayMask::AllDays; }
		if (IEquals(token, "Weekdays")) { return DayMask::Weekdays; }
		if (IEquals(token, "Weekends")) { return DayMask::Weekends; }
		if (IEquals(token, "Mon"))      { return DayMask::Monday; }
		if (IEquals(token, "Tue"))      { return DayMask::Tuesday; }
		if (IEquals(token, "Wed"))      { return DayMask::Wednesday; }
		if (IEquals(token, "Thu"))      { return DayMask::Thursday; }
		if (IEquals(token, "Fri"))      { return DayMask::Friday; }
		if (IEquals(token, "Sat"))      { return DayMask::Saturday; }
		if (IEquals(token, "Sun"))      { return DayMask::Sunday; }
		return std::nullopt;
	}

	std::optional<Scheduling::ControllerSchedule> ParseProgramDetailLines(
		const std::vector<std::string>& lines,
		int* out_program_index,
		int* out_program_count)
	{
		// The detail page fixes the target on line 0, "Pgm N of M" on line 2, ON on 3,
		// OFF on 4, and the days on 5. A page with fewer than 6 lines cannot be a
		// program-detail page.
		if (lines.size() < 6) { return std::nullopt; }

		const std::string_view target = Trim(lines[0]);
		if (target.empty()) { return std::nullopt; }

		// A "No Programs" placeholder (equipment with no schedule) is NOT a program
		// detail: its rows carry "No Programs" / "Add Program" / "New Program" instead of
		// the ON/OFF/days triple, so the ON/OFF/days parse below fails and we return
		// nullopt. Reject an explicit "No Programs" target line early as well.
		if (IEquals(target, "No Programs")) { return std::nullopt; }

		Scheduling::ControllerSchedule schedule;

		// "Pgm N of M" is informational; a missing/garbled index simply defaults to 1-of-1
		// (some renders blank the row mid-transition). It does not by itself invalidate the
		// page - the ON/OFF/days triple is the ground truth for "this is a program".
		int program_index = 1;
		int program_count = 1;
		ParseProgramIndexRow(lines[2], program_index, program_count);

		if (!ParseTimeRow(lines[3], "ON",  schedule.on_hour,  schedule.on_minute))  { return std::nullopt; }
		if (!ParseTimeRow(lines[4], "OFF", schedule.off_hour, schedule.off_minute)) { return std::nullopt; }

		const auto days = ParseDaysRow(lines[5]);
		if (!days.has_value()) { return std::nullopt; }
		schedule.days_of_week = days.value();

		schedule.target = std::string(target);
		schedule.enabled = true; // the controller offers delete only, never disable

		if (nullptr != out_program_index) { *out_program_index = program_index; }
		if (nullptr != out_program_count) { *out_program_count = program_count; }

		return schedule;
	}

	std::optional<Scheduling::ControllerSchedule> ParseProgramDetailPage(
		const Utility::ScreenDataPage& page,
		int* out_program_index,
		int* out_program_count)
	{
		std::vector<std::string> lines;
		lines.reserve(page.Size());
		for (std::size_t i = 0; i < page.Size(); ++i)
		{
			lines.push_back(page[i].Text);
		}
		return ParseProgramDetailLines(lines, out_program_index, out_program_count);
	}

}
// namespace AqualinkAutomate::Devices::OneTouch
