#include "formatters/circular_buffer_standard_formatter.h"


namespace AqualinkAutomate
{

	std::ostream& operator<<(std::ostream& os, const boost::circular_buffer<uint8_t>& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate
