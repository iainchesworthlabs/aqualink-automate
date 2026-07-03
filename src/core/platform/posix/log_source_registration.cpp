#include "application/log_source_registration.h"

namespace AqualinkAutomate::Application
{

	// POSIX has no OS-native event-source registry (the equivalent of the Windows
	// EventLog\Application\<source> key). Report Unsupported so the CLI handler can
	// print the "only supported on Windows" notice without an OS #ifdef.
	// See docs/platform-isolation.md.

	LogSourceRegistrationResult RegisterLogSource(const std::string& /*source_name*/)
	{
		return LogSourceRegistrationResult::Unsupported;
	}

	LogSourceRegistrationResult UnregisterLogSource(const std::string& /*source_name*/)
	{
		return LogSourceRegistrationResult::Unsupported;
	}

}
// namespace AqualinkAutomate::Application
