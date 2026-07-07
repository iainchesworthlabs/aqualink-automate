#pragma once

#include <expected>
#include <format>
#include <ostream>
#include <string>

#include "core/formatters/temperature_formatter.h"
#include "utility/string_conversion/temperature_string_converter.h"

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Utility::TemperatureStringConverter& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Utility::TemperatureStringConverter> : std::formatter<AqualinkAutomate::Kernel::Temperature>
{
	constexpr auto parse(std::format_parse_context& ctx) -> std::format_parse_context::iterator
	{
		return std::formatter<AqualinkAutomate::Kernel::Temperature>::parse(ctx);
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Utility::TemperatureStringConverter& temperature, FormatContext& ctx) const
	{
		std::ranges::copy(std::string_view{ "TEMP=" }, ctx.out());

		try
		{
			return std::formatter<AqualinkAutomate::Kernel::Temperature>::format(temperature().value(), ctx);
		}
		catch (const std::bad_expected_access<boost::system::error_code>& ex_bea)
		{
			return std::ranges::copy(std::string_view{ "??" }, ctx.out()).out;
		}
	}
};
