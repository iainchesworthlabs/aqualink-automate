#include <exception>
#include <format>

#include <syslog.h>

#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging.h"
#include "logging/logging_attributes.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"
#include "logging/sinks/sink_native.h"

//
// Linux implementation of MakeNativeSink: the POSIX syslog() C API (journald picks
// it up on systemd hosts). OS-specific, so it lives in the platform/ tree (wired by
// if(LINUX)). Deliberately NOT boost::log's stock syslog_backend: the vcpkg
// Boost.Log builds ship WITHOUT native-syslog support, so that backend falls back to
// a UDP/boost::asio implementation whose process-global asio service is torn down out
// of order at exit and corrupts the heap (glibc aborts; MSVC tolerated it). Calling
// syslog() directly owns no global asio state and tears down cleanly.
//

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
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

		// The facility is OR-ed into each syslog() call (not set process-globally via
		// openlog), so a general daemon sink and the audit authpriv sink can coexist
		// without clobbering each other's facility.
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
	}
	// namespace (anonymous)

	boost::shared_ptr<boost::log::sinks::sink> MakeNativeSink(const NativeSinkConfig& config)
	{
		namespace sinks = boost::log::sinks;
		namespace expr = boost::log::expressions;

		try
		{
			auto backend = boost::make_shared<NativeSyslogBackend>(ToPosixFacility(config.Facility));
			auto sink = boost::make_shared<sinks::synchronous_sink<NativeSyslogBackend>>(backend);

			sink->set_filter(config.Filter);
			sink->set_formatter(expr::stream << expr::smessage);

			return sink;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, std::format("Could not construct the syslog sink ({}); continuing without it", ex.what()));
			return {};
		}
	}

}
// namespace AqualinkAutomate::Logging::Sinks
