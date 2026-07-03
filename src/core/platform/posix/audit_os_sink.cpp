#include <format>
#include <string>

#include <syslog.h>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "auth/audit_log.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auth
{

	namespace
	{
		// A minimal Boost.Log sink backend over the POSIX syslog() C API
		// (journald picks this up on systemd hosts).
		//
		// Deliberately NOT boost::log's stock syslog_backend: the vcpkg
		// Boost.Log builds ship WITHOUT native-syslog support, so that backend
		// always falls back to its UDP/boost::asio implementation, which
		// registers a service into a process-global boost::asio
		// execution_context. That service is torn down out of order at process
		// exit (after asio's own state is gone), corrupting the heap — observed
		// as a mismatched-size delete / read-past-block at __run_exit_handlers,
		// crashing on glibc while MSVC tolerated it. Calling syslog() directly
		// owns no global asio state and tears down cleanly.
		class NativeSyslogBackend :
			public boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding>
		{
		public:
			NativeSyslogBackend()
			{
				// The ident string must outlive the log connection; a string
				// literal has static storage, so this is safe.
				::openlog("Aqualink-Automate", LOG_PID | LOG_NDELAY, LOG_USER);
			}

			~NativeSyslogBackend()
			{
				::closelog();
			}

			void consume(boost::log::record_view const&, string_type const& formatted)
			{
				::syslog(LOG_NOTICE, "%s", formatted.c_str());
			}
		};
	}
	// anonymous namespace

	boost::shared_ptr<boost::log::sinks::sink> RegisterAuditOsSink()
	{
		namespace expr = boost::log::expressions;
		namespace sinks = boost::log::sinks;

		try
		{
			auto sink = boost::make_shared<sinks::synchronous_sink<NativeSyslogBackend>>();

			sink->set_filter(channel == Channel::Audit);
			sink->set_formatter(expr::stream << expr::smessage);

			boost::log::core::get()->add_sink(sink);

			return sink;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Audit, std::format("Could not register the OS-native audit sink ({}); the JSONL audit file remains the durable trail", ex.what()));
			return {};
		}
	}

}
// namespace AqualinkAutomate::Auth
