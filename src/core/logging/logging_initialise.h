#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "logging/logging_formatter.h"
#include "logging/sinks/log_environment.h"
#include "logging/sinks/sink_native.h"

namespace AqualinkAutomate::Logging
{

	// Bootstrap the logging core with the auto-resolved console sink. Called before
	// options are parsed so start-up diagnostics are visible. `format` lets main pass
	// an early --log-format (from a lightweight argv pre-scan) so that even the
	// pre-options bootstrap lines are JSON when JSON was requested — otherwise a
	// container pipeline would see a few non-JSON startup lines.
	void Initialise(LogFormat format = LogFormat::Text);

	// The resolved sink configuration applied after options are processed. Built by
	// the composition root (main) from the Logging options + the detected
	// environment, so this layer stays free of any dependency on the options layer.
	struct RuntimeConfig
	{
		Sinks::SinkSelection Selection;
		Sinks::SyslogFacility GeneralNativeFacility = Sinks::SyslogFacility::Daemon;

		// Wire format for the console + file sinks (the native sink is message-only).
		LogFormat Format = LogFormat::Text;

		// File sink: only installed when Selection.File is set and a path is present.
		std::optional<std::filesystem::path> LogFilePath;
		std::uintmax_t LogFileMaxBytes = 10ULL * 1024ULL * 1024ULL;
		std::size_t LogFileMaxFiles = 5;
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
