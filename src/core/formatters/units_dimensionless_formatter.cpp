#include "formatters/units_dimensionless_formatter.h"


namespace AqualinkAutomate
{

	auto operator<<(std::ostream& os, const AqualinkAutomate::Units::ppm_quantity& obj) -> std::ostream&
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate
