#include "formatters/temperature_formatter.h"

#include "formatters/formatter_helpers.h"


namespace AqualinkAutomate::Kernel
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::Temperature& obj)
	{
		return AqualinkAutomate::Formatters::WriteFormatted(os, obj);
	}

}
// namespace AqualinkAutomate::Kernel
