#include "formatters/jandy_device_formatters.h"

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Devices::JandyDeviceId& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Devices::JandyDeviceType& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace std
