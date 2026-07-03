#include <string>
#include <vector>

#include <boost/any.hpp>

#include "logging/logging_channels.h"
#include "options/validators/syslog_facility_validator.h"
#include "options/validators/validate_enum_option.h"

namespace AqualinkAutomate::Logging::Sinks
{

	void validate(boost::any& v, std::vector<std::string> const& values, SyslogFacility* /* target_type */, int)
	{
		// Delegate to the shared, empty-safe, case-insensitive enum validator.
		// (SyslogFacility enumerators are PascalCase, e.g. daemon -> Daemon,
		// authpriv -> AuthPriv, local0 -> Local0.)
		Options::Validators::ValidateEnumOption<SyslogFacility>(v, values, AqualinkAutomate::Logging::Channel::Main, "syslog facility");
	}

}
// namespace AqualinkAutomate::Logging::Sinks
