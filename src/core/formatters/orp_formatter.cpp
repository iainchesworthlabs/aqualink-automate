#include "formatters/orp_formatter.h"
#include "formatters/units_electric_potential_formatter.h"


namespace AqualinkAutomate::Kernel
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::ORP& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate::Kernel
