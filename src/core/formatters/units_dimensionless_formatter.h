#pragma once

#include <format>
#include <ostream>

#include "formatters/formatter_helpers.h"
#include "types/units_dimensionless.h"


namespace AqualinkAutomate
{

	auto operator<<(std::ostream& os, const AqualinkAutomate::Units::ppm_quantity& obj) -> std::ostream&;

}
// namespace AqualinkAutomate

template<>
struct std::formatter<AqualinkAutomate::Units::ppm_quantity>
	: AqualinkAutomate::Formatters::QuantityFormatter<AqualinkAutomate::Units::ppm_quantity, " ppm">
{
};
