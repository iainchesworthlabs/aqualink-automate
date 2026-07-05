#include <charconv>
#include <format>
#include <string>

#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Scheduling
{

	namespace
	{
		// Parse "HH:MM" (24-hour) into hour/minute; false on any malformed field.
		bool ParseHhMm(const std::string& text, int& out_hour, int& out_minute)
		{
			const auto colon = text.find(':');
			if (colon == std::string::npos) { return false; }

			const auto h = std::string_view(text).substr(0, colon);
			const auto m = std::string_view(text).substr(colon + 1);
			int hour = 0, minute = 0;
			if (h.empty() || m.empty()) { return false; }
			if (std::from_chars(h.data(), h.data() + h.size(), hour).ec != std::errc{}) { return false; }
			if (std::from_chars(m.data(), m.data() + m.size(), minute).ec != std::errc{}) { return false; }
			if (hour < 0 || hour > 23 || minute < 0 || minute > 59) { return false; }
			out_hour = hour;
			out_minute = minute;
			return true;
		}
	}

	std::string_view ControllerScheduleStatusToString(ControllerScheduleStatus status)
	{
		using enum ControllerScheduleStatus;

		switch (status)
		{
		case Available:      return "available";
		case PendingCapture: return "pending_capture";
		case Unsupported:    return "unsupported";
		}
		return "pending_capture";
	}

	nlohmann::json ToJson(const ControllerSchedule& schedule)
	{
		return nlohmann::json{
			{ "id", schedule.id },
			{ "name", schedule.name },
			{ "target", schedule.target },
			{ "group", schedule.group },
			{ "enabled", schedule.enabled },
			{ "days_of_week", schedule.days_of_week },
			{ "on_local", std::format("{:02d}:{:02d}", schedule.on_hour, schedule.on_minute) },
			{ "off_local", std::format("{:02d}:{:02d}", schedule.off_hour, schedule.off_minute) },
		};
	}

	std::optional<ControllerSchedule> ControllerScheduleFromJson(const nlohmann::json& json, std::string& error)
	{
		if (!json.is_object())
		{
			error = "body must be a JSON object";
			return std::nullopt;
		}

		ControllerSchedule schedule;

		if (!json.contains("target") || !json["target"].is_string() || json["target"].get<std::string>().empty())
		{
			error = "target is required and must be a non-empty string";
			return std::nullopt;
		}
		schedule.target = json["target"].get<std::string>();

		if (!json.contains("days_of_week") || !json["days_of_week"].is_number_integer())
		{
			error = "days_of_week is required and must be an integer bitmask";
			return std::nullopt;
		}
		const auto days = json["days_of_week"].get<int>();
		if (days < 0 || days > 0x7f)
		{
			error = "days_of_week must be in the range 0..127";
			return std::nullopt;
		}
		schedule.days_of_week = static_cast<std::uint8_t>(days);

		if (!json.contains("on_local") || !json["on_local"].is_string() || !ParseHhMm(json["on_local"].get<std::string>(), schedule.on_hour, schedule.on_minute))
		{
			error = "on_local is required and must be a valid \"HH:MM\" time";
			return std::nullopt;
		}
		if (!json.contains("off_local") || !json["off_local"].is_string() || !ParseHhMm(json["off_local"].get<std::string>(), schedule.off_hour, schedule.off_minute))
		{
			error = "off_local is required and must be a valid \"HH:MM\" time";
			return std::nullopt;
		}

		if (json.contains("name") && json["name"].is_string()) { schedule.name = json["name"].get<std::string>(); }
		if (json.contains("group") && json["group"].is_string()) { schedule.group = json["group"].get<std::string>(); }
		schedule.enabled = true;
		return schedule;
	}

}
// namespace AqualinkAutomate::Scheduling
