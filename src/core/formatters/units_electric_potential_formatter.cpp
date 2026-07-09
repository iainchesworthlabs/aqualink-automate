#include "formatters/units_electric_potential_formatter.h"

#include "formatters/formatter_helpers.h"


namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Units::millivolt_quantity& obj)
	{
		return AqualinkAutomate::Formatters::WriteFormatted(os, obj);
	}

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Units::volt_quantity& obj)
	{
		return AqualinkAutomate::Formatters::WriteFormatted(os, obj);
	}

}
// namespace std
