#pragma once

#include "logging/sinks/log_environment.h"
#include "logging/sinks/sink_native.h"

namespace AqualinkAutomate::Logging
{

	// Bootstrap the logging core with the auto-resolved console sink. Called before
	// options are parsed so start-up diagnostics are visible.
	void Initialise();

	// The resolved sink configuration applied after options are processed. Built by
	// the composition root (main) from the Logging options + the detected
	// environment, so this layer stays free of any dependency on the options layer.
	struct RuntimeConfig
	{
		Sinks::SinkSelection Selection;
		Sinks::SyslogFacility GeneralNativeFacility = Sinks::SyslogFacility::Daemon;
	};

	// Replace the operational sink set with the one described by config: drops the
	// bootstrap console and installs console/native per the selection. The audit sink
	// is separate (installed by the auth bootstrap) and is not touched here.
	void Reconfigure(const RuntimeConfig& config);

	// Flush and remove all registered sinks. Wired into the ordered shutdown so any
	// async sink frontend delivers its queued records before the process exits.
	void Shutdown();

}
// namespace AqualinkAutomate::Logging
