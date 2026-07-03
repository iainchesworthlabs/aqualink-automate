#include <format>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/event_log_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "auth/audit_log.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auth
{

	boost::shared_ptr<boost::log::sinks::sink> RegisterAuditOsSink()
	{
		namespace expr = boost::log::expressions;
		namespace sinks = boost::log::sinks;

		try
		{
			// Windows Event Log (Application log, source = the app name).
			auto backend = boost::make_shared<sinks::simple_event_log_backend>(boost::log::keywords::log_source = "Aqualink-Automate");
			auto sink = boost::make_shared<sinks::synchronous_sink<sinks::simple_event_log_backend>>(backend);

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
