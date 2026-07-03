#pragma once

#include <string>

namespace AqualinkAutomate::Application
{

	//
	// Outcome of a log-source registration action. `Unsupported` is returned on every
	// platform without an OS-native event-source registry (i.e. everything but
	// Windows), so callers branch on this result instead of an OS #ifdef
	// (see docs/platform-isolation.md).
	//
	enum class LogSourceRegistrationResult
	{
		Succeeded,    // the registry key was written / removed
		Failed,       // supported here, but the action failed (e.g. not elevated)
		Unsupported,  // this platform has no event-source registration concept
	};

	//
	// Windows Event Log source registration (a one-time, elevated install step). The
	// runtime Event Log sink is built with registration_mode = never so an
	// unprivileged run never needs to write HKLM (docs/logging-sinks-redesign.md §3.3,
	// §10.4); registering the source here writes the EventLog\Application\<source> key
	// (EventMessageFile + TypesSupported) so Event Viewer treats it as a known source.
	//
	// The Windows implementation lives in platform/windows/log_source_registration.cpp;
	// every other platform gets the Unsupported stub in
	// platform/posix/log_source_registration.cpp (CMake selects which compiles).
	//
	LogSourceRegistrationResult RegisterLogSource(const std::string& source_name);
	LogSourceRegistrationResult UnregisterLogSource(const std::string& source_name);

}
// namespace AqualinkAutomate::Application
