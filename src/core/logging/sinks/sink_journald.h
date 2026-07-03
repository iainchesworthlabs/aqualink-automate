#pragma once

// The journald sink exists only on Linux, where libsystemd is resolved at RUNTIME
// via dlopen (no build-time libsystemd-dev, no link dependency). Elsewhere this
// header is empty; callers guard use of MakeJournaldSink with the same macro.
#if defined(__linux__)

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

	// True iff libsystemd.so.0 is present and sd_journal_sendv resolves at runtime.
	// The auto policy uses this to decide between the journald sink and console+"<N>".
	[[nodiscard]] bool IsJournaldAvailable();

	//
	// Build (but do NOT install) a native journald sink: each record is delivered via
	// the dlopen'd sd_journal_sendv with structured fields (PRIORITY, SYSLOG_IDENTIFIER,
	// MESSAGE, AA_CHANNEL, and CODE_FILE/CODE_LINE for Trace/Debug). Returns null if
	// journald is not available (libsystemd absent), so the caller can fall back.
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeJournaldSink(const JournaldSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks

#endif // __linux__
