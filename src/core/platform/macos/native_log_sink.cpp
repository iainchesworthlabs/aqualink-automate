#include <os/log.h>

#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_native.h"

//
// macOS implementation of MakeNativeSink: the unified logging system (os_log). OS-
// specific, so it lives in the platform/ tree (wired by if(APPLE)). The syslog
// facility in NativeSinkConfig is not meaningful here and is ignored.
//

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		[[nodiscard]] os_log_type_t ToOsLogType(Severity severity) noexcept
		{
			switch (severity)
			{
			case Severity::Trace:   return OS_LOG_TYPE_DEBUG;
			case Severity::Debug:   return OS_LOG_TYPE_DEBUG;
			case Severity::Info:    return OS_LOG_TYPE_INFO;
			case Severity::Notify:  return OS_LOG_TYPE_DEFAULT;
			case Severity::Warning: return OS_LOG_TYPE_DEFAULT;  // os_log has no distinct warning level
			case Severity::Error:   return OS_LOG_TYPE_ERROR;
			case Severity::Fatal:   return OS_LOG_TYPE_FAULT;
			}

			return OS_LOG_TYPE_DEFAULT;
		}

		class OsLogBackend : public boost::log::sinks::basic_formatted_sink_backend<char>
		{
		public:
			OsLogBackend() :
				m_Log(os_log_create("org.aqualinkautomate", "operational"))
			{
			}

			void consume(boost::log::record_view const& rec, string_type const& formatted_message)
			{
				const auto record_severity = rec[severity].get<Severity>();
				// %{public}s: do not redact the message text in the unified log.
				os_log_with_type(m_Log, ToOsLogType(record_severity), "%{public}s", formatted_message.c_str());
			}

		private:
			os_log_t m_Log;
		};
	}
	// namespace (anonymous)

	boost::shared_ptr<boost::log::sinks::sink> MakeNativeSink(const NativeSinkConfig& config)
	{
		namespace sinks = boost::log::sinks;
		namespace expr = boost::log::expressions;

		using oslog_sink = sinks::synchronous_sink<OsLogBackend>;
		auto sink = boost::make_shared<oslog_sink>(boost::make_shared<OsLogBackend>());

		sink->set_filter(config.Filter);
		sink->set_formatter(expr::stream << expr::smessage);

		return sink;
	}

}
// namespace AqualinkAutomate::Logging::Sinks
