#pragma once

#include <string>

namespace AqualinkAutomate::Application
{

	//
	// Windows Event Log source registration (a one-time, elevated install step). The
	// runtime Event Log sink is built with registration_mode = never so an
	// unprivileged run never needs to write HKLM (docs/logging-sinks-redesign.md §3.3,
	// §10.4); registering the source here writes the EventLog\Application\<source> key
	// (EventMessageFile + TypesSupported) so Event Viewer treats it as a known source.
	//
	// Implemented only on Windows (platform/windows/log_source_registration.cpp).
	// Returns true on success, false on failure (e.g. not elevated).
	//
	bool RegisterLogSource(const std::string& source_name);
	bool UnregisterLogSource(const std::string& source_name);

}
// namespace AqualinkAutomate::Application
