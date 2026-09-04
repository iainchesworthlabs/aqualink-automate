#include "formatters/chemistry_formatter.h"

namespace AqualinkAutomate::Utility
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Utility::ChemistryStringConverter& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate::Utility
