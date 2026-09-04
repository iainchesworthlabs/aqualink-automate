#pragma once

#include <format>
#include <ostream>

#include "formatters/formatter_helpers.h"
#include "types/units_electric_potential.h"


namespace AqualinkAutomate
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Units::millivolt_quantity& obj);
	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Units::volt_quantity& obj);

}
// namespace AqualinkAutomate

template<>
struct std::formatter<AqualinkAutomate::Units::millivolt_quantity>
	: AqualinkAutomate::Formatters::QuantityFormatter<AqualinkAutomate::Units::millivolt_quantity, "mV">
{
};

template<>
struct std::formatter<AqualinkAutomate::Units::volt_quantity>
	: AqualinkAutomate::Formatters::QuantityFormatter<AqualinkAutomate::Units::volt_quantity, "V">
{
};
