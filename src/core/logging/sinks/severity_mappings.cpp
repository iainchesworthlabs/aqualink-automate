#include <format>
#include <string>

#include "logging/sinks/severity_mappings.h"

namespace AqualinkAutomate::Logging::Sinks
{

	std::string JournaldPrefix(Severity severity)
	{
		return std::format("<{}>", SyslogPriorityValue(severity));
	}

}
// namespace AqualinkAutomate::Logging::Sinks
