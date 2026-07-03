#pragma once

#include <string>
#include <vector>

#include <boost/any.hpp>

#include "logging/sinks/sink_native.h"

namespace AqualinkAutomate::Logging::Sinks
{

	// boost::program_options custom validator for the SyslogFacility enum, found by
	// ADL on the target type. Delegates to the shared case-insensitive enum
	// validator (daemon/DAEMON -> Daemon, authpriv -> AuthPriv, local0 -> Local0).
	void validate(boost::any& v, std::vector<std::string> const& values, SyslogFacility* target_type, int);

}
// namespace AqualinkAutomate::Logging::Sinks
