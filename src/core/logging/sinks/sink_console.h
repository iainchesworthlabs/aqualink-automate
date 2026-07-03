#pragma once

#include <ostream>

#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging_formatter.h"

namespace AqualinkAutomate::Logging::Sinks
{

	struct ConsoleSinkConfig
	{
		// Stream the console writes to. Null (the default) => std::clog with a
		// null_deleter — the process-lifetime, unowned stderr target the logging
		// facade has always used. Tests pass a std::ostringstream to capture output.
		boost::shared_ptr<std::ostream> Stream;

		// Prepend the sd-daemon "<N>" priority prefix to each record. Enabled when
		// stderr is journald-connected (§5.2): journald strips the prefix and records
		// the real priority. Only the first line of a multi-line record carries it.
		bool JournaldPrefixes = false;

		// Text (human) or JSON-lines (pipelines). The "<N>" prefix, when enabled,
		// precedes either body.
		LogFormat Format = LogFormat::Text;
	};

	//
	// Build (but do NOT install) the console sink. The caller installs it via
	// SinkRegistry::Add. Carries the same per-channel severity filter as the rest of
	// the core and flushes after every record (container/pipe/crash-tail safety).
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeConsoleSink(const ConsoleSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks
