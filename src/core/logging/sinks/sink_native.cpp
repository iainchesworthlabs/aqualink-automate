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
#include <syslog.h>
#include <boost/log/sinks/basic_sink_backend.hpp>
#endif

#include "logging/logging.h"
#include "logging/logging_attributes.h"
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
		[[nodiscard]] int ToPosixFacility(SyslogFacility facility) noexcept
		{
			switch (facility)
			{
			case SyslogFacility::Daemon:   return LOG_DAEMON;
			case SyslogFacility::User:     return LOG_USER;
			case SyslogFacility::AuthPriv: return LOG_AUTHPRIV;
			case SyslogFacility::Local0:   return LOG_LOCAL0;
			case SyslogFacility::Local1:   return LOG_LOCAL1;
			case SyslogFacility::Local2:   return LOG_LOCAL2;
			case SyslogFacility::Local3:   return LOG_LOCAL3;
			case SyslogFacility::Local4:   return LOG_LOCAL4;
			case SyslogFacility::Local5:   return LOG_LOCAL5;
			case SyslogFacility::Local6:   return LOG_LOCAL6;
			case SyslogFacility::Local7:   return LOG_LOCAL7;
			}

			return LOG_DAEMON;
		}

		//
		// A minimal Boost.Log backend over the POSIX syslog() C API (journald picks it
		// up on systemd hosts). Deliberately NOT boost::log's stock syslog_backend: the
		// vcpkg Boost.Log builds ship WITHOUT native-syslog support, so that backend
		// falls back to a UDP/boost::asio implementation whose process-global asio
		// service is torn down out of order at exit and corrupts the heap (glibc
		// aborts; MSVC tolerated it). Calling syslog() directly owns no global asio
		// state and tears down cleanly. The facility is OR-ed into each syslog() call
		// (not set process-globally via openlog), so a general daemon sink and the
		// audit authpriv sink can coexist without clobbering each other's facility.
		//
		class NativeSyslogBackend : public boost::log::sinks::basic_formatted_sink_backend<char>
		{
		public:
			explicit NativeSyslogBackend(int facility) :
				m_Facility(facility)
			{
				// The ident string must outlive the connection; a string literal has
				// static storage, so this is safe.
				::openlog("Aqualink-Automate", LOG_PID | LOG_NDELAY, LOG_DAEMON);
			}

			~NativeSyslogBackend()
			{
				::closelog();
			}

			void consume(boost::log::record_view const& rec, string_type const& formatted_message)
			{
				const auto record_severity = rec[severity].get<Severity>();
				// SyslogPriorityValue() is the syslog level 0..7 (== LOG_EMERG..LOG_DEBUG);
				// OR in this sink's facility so it is honoured per-record.
				::syslog(m_Facility | SyslogPriorityValue(record_severity), "%s", formatted_message.c_str());
			}

		private:
			int m_Facility;
		};
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
			auto backend = boost::make_shared<NativeSyslogBackend>(ToPosixFacility(config.Facility));
			auto sink = boost::make_shared<sinks::synchronous_sink<NativeSyslogBackend>>(backend);
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
