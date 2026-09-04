#include "formatters/jandy_device_formatters.h"

namespace AqualinkAutomate::Devices
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
// namespace AqualinkAutomate::Devices
