#include <array>
#include <charconv>
#include <format>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "kernel/circulation.h"
#include "scheduling/schedule.h"

namespace AqualinkAutomate::Scheduling
{
	namespace
	{
		struct ActionNamePair { ActionType type; std::string_view name; };

		constexpr std::array<ActionNamePair, 7> ACTION_NAMES
		{ {
			{ ActionType::ButtonOn, "button_on" },
			{ ActionType::ButtonOff, "button_off" },
			{ ActionType::ButtonToggle, "button_toggle" },
			{ ActionType::PoolSetpoint, "pool_setpoint" },
			{ ActionType::SpaSetpoint, "spa_setpoint" },
			{ ActionType::ChlorinatorPercent, "chlorinator_percent" },
			{ ActionType::CirculationMode, "circulation_mode" },
		} };

		bool IsButtonAction(ActionType type)
		{
			using enum ActionType;
			return type == ButtonOn || type == ButtonOff || type == ButtonToggle;
		}

		bool IsSetpointAction(ActionType type)
		{
			return type == ActionType::PoolSetpoint || type == ActionType::SpaSetpoint;
		}

		// Parse "HH:MM" into hour/minute. Returns false on any malformation.
		bool ParseTime(std::string_view text, int& hour, int& minute)
		{
			const auto colon = text.find(':');
			if (colon == std::string_view::npos) { return false; }

			const std::string_view h = text.substr(0, colon);
			const std::string_view m = text.substr(colon + 1);
			if (h.empty() || m.empty()) { return false; }

			int hv = 0;
			int mv = 0;
			if (std::from_chars(h.data(), h.data() + h.size(), hv).ec != std::errc{}) { return false; }
			if (std::from_chars(m.data(), m.data() + m.size(), mv).ec != std::errc{}) { return false; }
			if (hv < 0 || hv > 23 || mv < 0 || mv > 59) { return false; }

			hour = hv;
			minute = mv;
			return true;
		}
	}
	// unnamed namespace

	std::string_view ActionTypeToString(ActionType type)
	{
		for (const auto& pair : ACTION_NAMES)
		{
			if (pair.type == type) { return pair.name; }
		}
		return "button_toggle";
	}

	std::optional<ActionType> ActionTypeFromString(std::string_view text)
	{
		for (const auto& pair : ACTION_NAMES)
		{
			if (pair.name == text) { return pair.type; }
		}
		return std::nullopt;
	}

	nlohmann::json ToJson(const Schedule& schedule)
	{
		nlohmann::json action;
		action["type"] = std::string{ ActionTypeToString(schedule.action.type) };
		if (IsButtonAction(schedule.action.type) || schedule.action.type == ActionType::CirculationMode)
		{
			action["target"] = schedule.action.target;
		}
		else
		{
			action["value"] = schedule.action.value;
		}

		return nlohmann::json{
			{ "uuid", schedule.uuid },
			{ "name", schedule.name },
			{ "enabled", schedule.enabled },
			{ "days_of_week", schedule.days_of_week },
			{ "time_local", std::format("{:02d}:{:02d}", schedule.hour, schedule.minute) },
			{ "action", std::move(action) },
		};
	}

	std::optional<Schedule> FromJson(const nlohmann::json& json, std::string& error)
	{
		if (!json.is_object())
		{
			error = "schedule must be a JSON object";
			return std::nullopt;
		}

		Schedule schedule;

		if (json.contains("uuid") && json["uuid"].is_string())
		{
			schedule.uuid = json["uuid"].get<std::string>();
		}

		schedule.name = json.value("name", std::string{});
		schedule.enabled = json.value("enabled", true);

		if (!json.contains("days_of_week") || !json["days_of_week"].is_number_integer())
		{
			error = "days_of_week (integer bitmask) is required";
			return std::nullopt;
		}
		const std::int64_t dow = json["days_of_week"].get<std::int64_t>();
		if (dow < 0 || dow > 127)
		{
			error = "days_of_week must be a 0..127 bitmask";
			return std::nullopt;
		}
		schedule.days_of_week = static_cast<std::uint8_t>(dow);

		if (!json.contains("time_local") || !json["time_local"].is_string()
			|| !ParseTime(json["time_local"].get<std::string>(), schedule.hour, schedule.minute))
		{
			error = "time_local must be a valid \"HH:MM\" string";
			return std::nullopt;
		}

		if (!json.contains("action") || !json["action"].is_object())
		{
			error = "action object is required";
			return std::nullopt;
		}
		const auto& action_json = json["action"];

		auto type = ActionTypeFromString(action_json.value("type", std::string{}));
		if (!type.has_value())
		{
			error = "action.type is unknown";
			return std::nullopt;
		}
		schedule.action.type = type.value();

		if (IsButtonAction(*type))
		{
			schedule.action.target = action_json.value("target", std::string{});
			if (schedule.action.target.empty())
			{
				error = "action.target (device label) is required for button actions";
				return std::nullopt;
			}
		}
		else if (*type == ActionType::CirculationMode)
		{
			schedule.action.target = action_json.value("target", std::string{});
			if (!magic_enum::enum_cast<Kernel::CirculationModes>(schedule.action.target).has_value())
			{
				error = "action.target is not a valid circulation mode";
				return std::nullopt;
			}
		}
		else
		{
			if (!action_json.contains("value") || !action_json["value"].is_number_integer())
			{
				error = "action.value (integer) is required";
				return std::nullopt;
			}
			schedule.action.value = action_json["value"].get<int>();

			if (IsSetpointAction(*type) && (schedule.action.value < 1 || schedule.action.value > 104))
			{
				error = "setpoint value must be 1..104";
				return std::nullopt;
			}
			if (*type == ActionType::ChlorinatorPercent && (schedule.action.value < 0 || schedule.action.value > 100))
			{
				error = "chlorinator_percent value must be 0..100";
				return std::nullopt;
			}
		}

		return schedule;
	}

}
// namespace AqualinkAutomate::Scheduling
