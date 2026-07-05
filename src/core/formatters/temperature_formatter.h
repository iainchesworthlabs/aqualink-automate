#pragma once

#include <cstdint>
#include <format>
#include <ostream>
#include <string_view>

#include "kernel/temperature.h"

namespace AqualinkAutomate::Formatters
{

	// The degree symbol (U+00B0) emitted between the numeric value and the unit glyph.
	inline constexpr std::string_view TEMPERATURE_DEGREE_SYMBOL{ "\u{B0}" };

	// Maps a TemperatureUnits enumerator to its single-character unit glyph. The mapping is total
	// over the enum (only Celsius / Fahrenheit exist), so the formatter needs no unreachable
	// fallback unit; Celsius is the natural default for the canonical internal representation.
	[[nodiscard]] constexpr char TemperatureUnitGlyph(AqualinkAutomate::Kernel::TemperatureUnits units) noexcept
	{
		return (units == AqualinkAutomate::Kernel::TemperatureUnits::Fahrenheit) ? 'F' : 'C';
	}

}
// AqualinkAutomate::Formatters

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::Temperature& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Kernel::Temperature>
{
	// Set the formatting defaults
	AqualinkAutomate::Kernel::TemperatureUnits display_units{ AqualinkAutomate::Kernel::TemperatureUnits::Celsius };
	uint32_t display_precision{ 0 };

	constexpr auto parse(std::format_parse_context& ctx) -> std::format_parse_context::iterator
	{
		auto it = ctx.begin();

		if (it == ctx.end() || *it == '}')
		{
			return it;
		}

		// Parse optional precision: .N
		if (*it == '.')
		{
			++it;
			uint32_t precision = 0;
			while (it != ctx.end() && *it >= '0' && *it <= '9')
			{
				precision = precision * 10 + static_cast<uint32_t>(*it - '0');
				++it;
			}
			display_precision = precision;
		}

		if (it == ctx.end() || *it == '}')
		{
			return it;
		}

		if (*it == 'C')
		{
			display_units = AqualinkAutomate::Kernel::TemperatureUnits::Celsius;
			return ++it;
		}

		if (*it == 'F')
		{
			display_units = AqualinkAutomate::Kernel::TemperatureUnits::Fahrenheit;
			return ++it;
		}

		throw std::format_error("Unsupported temperature unit format specifier");
	}

	template<class FmtContext>
	auto format(const AqualinkAutomate::Kernel::Temperature& temperature, FmtContext& ctx) const -> FmtContext::iterator
	{
		const double temp_value =
			(display_units == AqualinkAutomate::Kernel::TemperatureUnits::Fahrenheit)
				? temperature.InFahrenheit().value()
				: temperature.InCelsius().value();

		// Output the value, the degrees symbol and the units glyph.
		return std::format_to(ctx.out(), "{:.{}f}{}{}", temp_value, display_precision, AqualinkAutomate::Formatters::TEMPERATURE_DEGREE_SYMBOL, AqualinkAutomate::Formatters::TemperatureUnitGlyph(display_units));
	}
};
