#include "formatters/stats_counter_formatter.h"

namespace AqualinkAutomate::Utility
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Utility::StatsCounter& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate::Utility
