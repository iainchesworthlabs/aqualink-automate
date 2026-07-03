#pragma once

// The os_log sink exists only on macOS/Apple platforms. Elsewhere this header is
// empty; callers guard use of MakeOsLogSink with the same macro.
#if defined(__APPLE__)

#include <boost/log/expressions/filter.hpp>
#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

namespace AqualinkAutomate::Logging::Sinks
{

	struct OsLogSinkConfig
	{
		// Which records this sink accepts (MakeOperationalFilter() for operational use).
		boost::log::filter Filter;
	};

	//
	// Build (but do NOT install) a native macOS os_log sink: each record is delivered
	// via os_log_with_type on a dedicated os_log_t handle, mapping Severity onto the
	// os_log_type_t levels. This is the modern native path on macOS, replacing the
	// syslog(3)/ASL shim used by MakeNativeSink on other POSIX platforms.
	//
	[[nodiscard]] boost::shared_ptr<boost::log::sinks::sink> MakeOsLogSink(const OsLogSinkConfig& config);

}
// namespace AqualinkAutomate::Logging::Sinks

#endif // __APPLE__
