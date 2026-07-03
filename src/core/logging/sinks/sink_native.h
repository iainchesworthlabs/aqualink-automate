#pragma once

#include <string>

#include <boost/log/expressions/filter.hpp>
#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

namespace AqualinkAutomate::Logging::Sinks
{

	//
	// POSIX syslog facility (docs/logging-sinks-redesign.md §10.3). The audit trail
	// uses AuthPriv (LOG_AUTHPRIV) so distros route it to a restricted-mode auth log;
	// the general operational native sink defaults to Daemon.
	//
	enum class SyslogFacility
	{
		Daemon,
		User,
		AuthPriv,
		Local0,
		Local1,
		Local2,
		Local3,
		Local4,
		Local5,
		Local6,
		Local7
	};

	struct NativeSinkConfig
	{
		// Which records this sink accepts (e.g. MakeAuditFilter() for the audit sink,
		// MakeOperationalFilter() for a general operational native sink).
		boost::log::filter Filter;

		// POSIX syslog facility. Ignored on Windows.
		SyslogFacility Facility = SyslogFacility::Daemon;

		// Windows Event Log source name. Ignored on POSIX.
		std::string WindowsEventSource{ "Aqualink-Automate" };
	};

	//
	// Build (but do NOT install) the OS-native sink: a syslog sink on POSIX (with the
	// configured facility) or a Windows Event Log sink, each carrying the explicit
	// Severity -> level/event-type mapping (§7). Returns a null handle — after logging
	// a warning on Channel::Main — when the platform sink cannot be constructed (e.g.
	// an unprivileged Windows run cannot register the event source, §3.3). The caller
	// installs the returned sink via SinkRegistry::Add / boost::log::core.
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeNativeSink(const NativeSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks
