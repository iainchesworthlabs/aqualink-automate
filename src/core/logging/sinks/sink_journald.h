#pragma once

#include <string>

#include <boost/log/expressions/filter.hpp>
#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

namespace AqualinkAutomate::Logging::Sinks
{

	struct JournaldSinkConfig
	{
		// Which records this sink accepts (MakeOperationalFilter() for operational use).
		boost::log::filter Filter;

		// SYSLOG_IDENTIFIER field value in the journal.
		std::string SyslogIdentifier{ "aqualink-automate" };
	};

	// True iff a journald sink can be built here: Linux with libsystemd.so.0 present
	// and sd_journal_sendv resolvable at runtime. False on Windows/macOS (stub impls)
	// and on Linux without libsystemd. The auto policy uses this to decide between the
	// journald sink and console+"<N>". Declared for every platform (the real impl is
	// platform/linux/journald_log_sink.cpp; Windows/macOS provide stubs) so callers
	// need no platform #ifdef.
	[[nodiscard]] bool IsJournaldAvailable();

	//
	// Build (but do NOT install) a native journald sink. On Linux each record is
	// delivered via the dlopen'd sd_journal_sendv with structured fields (PRIORITY,
	// SYSLOG_IDENTIFIER, MESSAGE, AA_CHANNEL, and CODE_FILE/CODE_LINE for Trace/Debug).
	// Returns null when journald is not available so the caller can fall back.
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeJournaldSink(const JournaldSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks
