#include "formatters/ph_formatter.h"


namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::pH& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace std
