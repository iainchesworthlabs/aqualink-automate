#include "formatters/jandy_device_formatters.h"
#include "formatters/jandy_message_formatters.h"

namespace AqualinkAutomate::Messages
{
	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Messages::JandyMessageIds& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace AqualinkAutomate::Messages
