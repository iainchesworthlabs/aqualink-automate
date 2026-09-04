#include "formatters/ph_formatter.h"


namespace AqualinkAutomate::Kernel
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::pH& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate::Kernel
