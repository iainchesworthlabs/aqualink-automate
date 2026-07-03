#pragma once

// The journald sink exists only where libsystemd was found (Linux/systemd). On
// every other platform SYSTEMD_SUPPORT_ENABLED is undefined and this header is
// empty, so callers must guard use of MakeJournaldSink with the same macro.
#if defined(SYSTEMD_SUPPORT_ENABLED)

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

	//
	// Build (but do NOT install) a native journald sink: each record is delivered via
	// sd_journal_send with structured fields (PRIORITY, SYSLOG_IDENTIFIER, MESSAGE,
	// AA_CHANNEL, and CODE_FILE/CODE_LINE for Trace/Debug). This is the richer upgrade
	// over the console "<N>" priority-prefix path (docs/logging-sinks-redesign.md §5.3):
	// journald records the real priority AND queryable fields (journalctl AA_CHANNEL=Web).
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeJournaldSink(const JournaldSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks

#endif // SYSTEMD_SUPPORT_ENABLED
