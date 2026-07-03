#include <exception>
#include <format>

#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <magic_enum/magic_enum_utility.hpp>

#if defined(_WIN32)
#include <boost/log/sinks/event_log_backend.hpp>
#elif !defined(__APPLE__)
#include <boost/log/sinks/syslog_backend.hpp>
#endif

#include "logging/logging.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"
#include "logging/sinks/sink_native.h"

#if defined(__APPLE__)
#include "logging/sinks/sink_oslog.h"
#endif

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		namespace sinks = boost::log::sinks;
		namespace expr = boost::log::expressions;

#if !defined(_WIN32) && !defined(__APPLE__)
		[[nodiscard]] sinks::syslog::facility ToBoostFacility(SyslogFacility facility) noexcept
		{
			switch (facility)
			{
			case SyslogFacility::Daemon:   return sinks::syslog::daemon;
			case SyslogFacility::User:     return sinks::syslog::user;
			// AuthPriv = LOG_AUTHPRIV: Boost's security1 is facility 10 on Linux (§10.3).
			case SyslogFacility::AuthPriv: return sinks::syslog::security1;
			case SyslogFacility::Local0:   return sinks::syslog::local0;
			case SyslogFacility::Local1:   return sinks::syslog::local1;
			case SyslogFacility::Local2:   return sinks::syslog::local2;
			case SyslogFacility::Local3:   return sinks::syslog::local3;
			case SyslogFacility::Local4:   return sinks::syslog::local4;
			case SyslogFacility::Local5:   return sinks::syslog::local5;
			case SyslogFacility::Local6:   return sinks::syslog::local6;
			case SyslogFacility::Local7:   return sinks::syslog::local7;
			}

			return sinks::syslog::daemon;
		}

		[[nodiscard]] sinks::syslog::level ToBoostLevel(SyslogLevel level) noexcept
		{
			// SyslogLevel's values are defined to match boost::log::sinks::syslog::level
			// (and the POSIX priorities) exactly — see severity_mappings.h.
			return static_cast<sinks::syslog::level>(static_cast<int>(level));
		}
#elif defined(_WIN32)
		[[nodiscard]] sinks::event_log::event_type ToBoostEventType(EventType type) noexcept
		{
			switch (type)
			{
			case EventType::Information: return sinks::event_log::info;
			case EventType::Warning:     return sinks::event_log::warning;
			case EventType::Error:       return sinks::event_log::error;
			}

			return sinks::event_log::info;
		}
#endif
	}
	// namespace (anonymous)

	boost::shared_ptr<boost::log::sinks::sink> MakeNativeSink(const NativeSinkConfig& config)
	{
#if defined(__APPLE__)
		// macOS: the native path is os_log (the syslog facility is not meaningful here).
		return MakeOsLogSink(OsLogSinkConfig{ .Filter = config.Filter });
#else
		namespace keywords = boost::log::keywords;

		try
		{
#if defined(_WIN32)
			// registration_mode = never: do NOT write the HKLM event-source registry
			// key at runtime. That write needs elevation and would throw on an
			// unprivileged run (losing the sink entirely, §3.3). The source is instead
			// registered once at install time (--register-log-source); an unregistered
			// source still logs to the Application log, just with a generic description
			// wrapper until registration + a message table are in place.
			auto backend = boost::make_shared<sinks::simple_event_log_backend>(
				keywords::log_source = config.WindowsEventSource,
				keywords::registration = sinks::event_log::never);

			sinks::event_log::custom_event_type_mapping<Severity> mapping("Severity");
			magic_enum::enum_for_each<Severity>([&mapping](Severity severity)
				{
					mapping[severity] = ToBoostEventType(ToEventType(severity));
				});
			backend->set_event_type_mapper(mapping);

			auto sink = boost::make_shared<sinks::synchronous_sink<sinks::simple_event_log_backend>>(backend);
#else
			auto backend = boost::make_shared<sinks::syslog_backend>(
				keywords::facility = ToBoostFacility(config.Facility),
				keywords::use_impl = sinks::syslog::native);

			sinks::syslog::custom_severity_mapping<Severity> mapping("Severity");
			magic_enum::enum_for_each<Severity>([&mapping](Severity severity)
				{
					mapping[severity] = ToBoostLevel(ToSyslogLevel(severity));
				});
			backend->set_severity_mapper(mapping);

			auto sink = boost::make_shared<sinks::synchronous_sink<sinks::syslog_backend>>(backend);
#endif

			sink->set_filter(config.Filter);
			sink->set_formatter(expr::stream << expr::smessage);

			return sink;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, std::format("Could not construct the OS-native log sink ({}); continuing without it", ex.what()));
			return {};
		}
#endif // __APPLE__
	}

}
// namespace AqualinkAutomate::Logging::Sinks
