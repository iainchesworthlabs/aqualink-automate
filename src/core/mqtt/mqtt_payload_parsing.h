#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "utility/to_number.h"

namespace AqualinkAutomate::Mqtt::PayloadParsing
{

	/// Maximum number of broker-supplied payload characters echoed into a log line.
	inline constexpr std::size_t MAX_LOGGED_PAYLOAD_LENGTH{ 64U };

	/// Sanitise an untrusted broker-supplied string for safe inclusion in a log line.
	/// Non-printable bytes are replaced with '?' and the result is truncated so a
	/// hostile/oversized payload cannot flood or corrupt the log.
	inline constexpr std::string SanitiseForLog(std::string_view value)
	{
		std::string result;
		result.reserve(std::min(value.size(), MAX_LOGGED_PAYLOAD_LENGTH));

		for (std::size_t index = 0U; (index < value.size()) && (index < MAX_LOGGED_PAYLOAD_LENGTH); ++index)
		{
			const auto byte = static_cast<unsigned char>(value[index]);
			result += (byte >= 0x20U && byte < 0x7FU) ? static_cast<char>(byte) : '?';
		}

		if (value.size() > MAX_LOGGED_PAYLOAD_LENGTH)
		{
			result += "...";
		}

		return result;
	}

	/// Parse a numeric value from a JSON payload (handles raw string, number, and string types).
	///
	/// Broker-controlled values are validated against the full numeric range of T using
	/// std::from_chars (via Utility::ToNumber) rather than std::stoi + a narrowing
	/// static_cast: an out-of-range value (e.g. "300" into a uint8_t) is REJECTED and the
	/// default returned, never silently wrapped/truncated. std::from_chars is also
	/// non-throwing, so a malformed payload no longer raises std::invalid_argument /
	/// std::out_of_range on the inbound MQTT hot path.
	template<typename T>
	T ParsePayloadNumber(const nlohmann::json& payload, T default_value = T{})
	{
		if (payload.contains("raw") && payload["raw"].is_string())
		{
			return Utility::ToNumber<T>(payload["raw"].get<std::string>()).value_or(default_value);
		}
		else if (payload.is_number())
		{
			// Range-check the JSON number against T's domain BEFORE conversion so a
			// broker-supplied out-of-range number cannot wrap/truncate (or invoke UB on
			// the double->integer cast).
			const double number = payload.get<double>();
			if (!std::isfinite(number) ||
				(number < static_cast<double>(std::numeric_limits<T>::lowest())) ||
				(number > static_cast<double>(std::numeric_limits<T>::max())))
			{
				return default_value;
			}

			if constexpr (std::floating_point<T>)
			{
				return static_cast<T>(number);
			}
			else
			{
				return static_cast<T>(std::llround(number));
			}
		}
		else if (payload.is_string())
		{
			return Utility::ToNumber<T>(payload.get<std::string>()).value_or(default_value);
		}

		return default_value;
	}

	/// Parse a string value from a JSON payload (handles raw object and plain string).
	inline std::string ParsePayloadString(const nlohmann::json& payload)
	{
		if (payload.contains("raw") && payload["raw"].is_string())
		{
			return payload["raw"].get<std::string>();
		}
		else if (payload.is_string())
		{
			return payload.get<std::string>();
		}

		return {};
	}

}
// namespace AqualinkAutomate::Mqtt::PayloadParsing
