#include <algorithm>
#include <charconv>
#include <format>
#include <limits>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "utility/string_conversion/temperature_string_converter.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{
	// Area label, separator, signed value, degree (`), unit. The area may be MULTIPLE words
	// (e.g. "Pool Heat" / "Spa Heat" on the Set Temperature page) so it allows internal spaces
	// via a non-greedy run (the trailing spaces fall to the \s separator); the value allows up
	// to THREE digits so spa temperatures/setpoints of 100`F+ parse. The ^...$ anchors keep it
	// a full-string match (regex_search), so trailing junk like "Pool 22`C extra" still fails.
	const std::string TemperatureStringConverter::REGEX_PATTERN{ R"(^([A-Za-z0-9][A-Za-z0-9 ]{0,14}?)\s{1,10}(-?\d{1,3})`([CF])$)" };
	const boost::regex TemperatureStringConverter::REGEX_PARSER{ REGEX_PATTERN };

	TemperatureStringConverter::TemperatureStringConverter() noexcept
	{
	}

	TemperatureStringConverter::TemperatureStringConverter(const std::string& temperature_string) noexcept :
		m_Temperature(Kernel::Temperature::ConvertToTemperatureInCelsius(0)),
		m_ErrorOccurred(std::nullopt)
	{
		ConvertStringToTemperature(TrimWhitespace(temperature_string));
	}

	TemperatureStringConverter& TemperatureStringConverter::operator=(const std::string& temperature_string) noexcept
	{
		ConvertStringToTemperature(TrimWhitespace(temperature_string));
		return *this;
	}

	std::expected<Kernel::Temperature, boost::system::error_code> TemperatureStringConverter::operator()() const noexcept
	{
		if (m_ErrorOccurred.has_value())
		{
			return std::unexpected<boost::system::error_code>(make_error_code(m_ErrorOccurred.value()));
		}

		return m_Temperature;
	}

	std::expected<std::string, boost::system::error_code> TemperatureStringConverter::TemperatureArea() const noexcept
	{
		if (m_ErrorOccurred.has_value())
		{
			return std::unexpected<boost::system::error_code>(make_error_code(m_ErrorOccurred.value()));
		}

		return m_TemperatureArea;
	}

	void TemperatureStringConverter::ConvertStringToTemperature(const std::string& temperature_string) noexcept
	{
		const auto temperature_data = ValidateAndExtractData(temperature_string);

		const auto temperature_area = std::get<0>(temperature_data);
		const auto temperature = std::get<1>(temperature_data);
		const auto temperature_units = std::get<2>(temperature_data);

		if (temperature_area && temperature && temperature_units)
		{
			int32_t converted_temperature = 0;

			auto [_, ec] = std::from_chars((*temperature).data(), (*temperature).data() + (*temperature).size(), converted_temperature);
			if (std::errc() != ec)
			{
				LogDebug(Channel::Devices, std::format("Failed to convert temperature; could not convert to number: error -> {}", magic_enum::enum_name(ec)));
				m_ErrorOccurred = ErrorCodes::StringConversion_ErrorCodes::MalformedInput;
			}
			else
			{
				switch ((*temperature_units)[0])
				{
				case 'C':
					m_Temperature = Kernel::Temperature::ConvertToTemperatureInCelsius(static_cast<double>(converted_temperature));
					break;
				case 'F':
					m_Temperature = Kernel::Temperature::ConvertToTemperatureInFahrenheit(static_cast<double>(converted_temperature));
					break;
				default:
					m_ErrorOccurred = ErrorCodes::StringConversion_ErrorCodes::MalformedInput;
					break;
				}

				m_TemperatureArea = TrimWhitespace(*temperature_area);
			}
		}
		else 
		{
			LogDebug(Channel::Devices, std::format("Failed to convert temperature; malformed input -> {}", temperature_string));
			m_ErrorOccurred = ErrorCodes::StringConversion_ErrorCodes::MalformedInput;
		}
	}

	std::tuple<std::optional<std::string>, std::optional<std::string>, std::optional<std::string>> TemperatureStringConverter::ValidateAndExtractData(const std::string& temperature_string)  noexcept
	{
		boost::smatch match_results;

		if (MINIMUM_STRING_LENGTH > temperature_string.size() || MAXIMUM_STRING_LENGTH < temperature_string.size() ||
			!boost::regex_search(temperature_string, match_results, REGEX_PARSER) ||
			3 > match_results.size())
		{
			// Invalid string length, pattern match, or insufficient resultset...do nothing.
			return std::make_tuple(std::nullopt, std::nullopt, std::nullopt);
		}

		return std::make_tuple<>(
			std::optional<std::string>(match_results[1]),
			std::optional<std::string>(match_results[2]),
			std::optional<std::string>(match_results[3])
		);
	}

}
// namespace AqualinkAutomate::Utility
