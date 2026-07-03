#include <exception>
#include <format>

#include <boost/log/expressions.hpp>
#include <boost/log/sinks/event_log_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <magic_enum/magic_enum_utility.hpp>

#include "logging/logging.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"
#include "logging/sinks/sink_native.h"

//
// Windows implementation of MakeNativeSink: the Application Event Log. OS-specific,
// so it lives in the platform/ tree (wired by if(WIN32) in src/core/CMakeLists.txt)
// rather than behind an #ifdef in a shared source.
//

namespace AqualinkAutomate::Logging::Sinks
{

	namespace
	{
		namespace sinks = boost::log::sinks;

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
	}
	// namespace (anonymous)

	boost::shared_ptr<boost::log::sinks::sink> MakeNativeSink(const NativeSinkConfig& config)
	{
		namespace expr = boost::log::expressions;
		namespace keywords = boost::log::keywords;

		try
		{
			// registration = never: do NOT write the HKLM event-source registry key at
			// runtime. That write needs elevation and would throw on an unprivileged run
			// (losing the sink entirely, §3.3). The source is registered once at install
			// time (--register-log-source); an unregistered source still logs to the
			// Application log, just with a generic description wrapper.
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

			sink->set_filter(config.Filter);
			sink->set_formatter(expr::stream << expr::smessage);

			return sink;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, std::format("Could not construct the Windows Event Log sink ({}); continuing without it", ex.what()));
			return {};
		}
	}

}
// namespace AqualinkAutomate::Logging::Sinks
