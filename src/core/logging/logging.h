#pragma once

#include <concepts>
#include <cstddef>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>

#include <magic_enum/magic_enum.hpp>

#include "logging/global_logger.h"
#include "logging/logging_attributes.h"
#include "logging/logging_severity_filter.h"

#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
#include "profiling/factories/profiler_factory.h"
#include "profiling/profiling_units/unit_colours.h"
#endif

namespace AqualinkAutomate::Logging
{

	//
	// Single source of truth for the number of channels. Adding a Channel enumerator
	// requires a matching BOOST_LOG_GLOBAL_LOGGER (global_logger.h/.cpp) and a case in
	// the GetGlobalLogger switch below; this static_assert fails the build if the
	// enumerator count drifts from the wired-up logger set, forcing both to be updated.
	//
	inline constexpr std::size_t CHANNEL_COUNT = magic_enum::enum_count<Channel>();
	static_assert(CHANNEL_COUNT == 18U, "Channel enum changed: update global_logger.{h,cpp} and the GetGlobalLogger() switch in logging.h to match.");

	//
	// A loggable message is either:
	//   * an invocable (lazy-formatting lambda) - the supported deferred-formatting form, or
	//   * a value that can itself be streamed into the Boost.Log record ostream.
	// Constraining Log()/Log*() this way turns a misuse (e.g. passing a non-streamable
	// object) into a clear compile error rather than a deep template-instantiation failure.
	//
	template<typename MESSAGE>
	concept Loggable =
		std::invocable<MESSAGE> ||
		requires(boost::log::record_ostream& os, MESSAGE&& m) { os << std::forward<MESSAGE>(m); };

	//
	// Compute the basename of a source path at the cheapest cost: a backwards scan for the
	// last path separator over the static, null-terminated string returned by
	// std::source_location::file_name(). This avoids constructing a std::filesystem::path
	// plus a heap std::string on every emitted record. The pointer remains valid because
	// file_name() returns a pointer into static program storage.
	//
	[[nodiscard]] constexpr std::string_view SourceFileBasename(const char* file_path) noexcept
	{
		const std::string_view path{ file_path };

		if (const auto separator = path.find_last_of("/\\"); separator != std::string_view::npos)
		{
			return path.substr(separator + 1U);
		}

		return path;
	}

	template<Loggable MESSAGE>
	void Log(MESSAGE&& log_message, Channel channel, Severity severity, const std::source_location location)
	{
		// Early-out: skip all work if this severity is below the channel's filter level
		if (!SeverityFiltering::ShouldLog(channel, severity))
		{
			return;
		}

		// Resolve the message: invoke callables (deferred formatting), pass through strings
		auto resolved = [&]()
		{
			if constexpr (std::invocable<MESSAGE>)
			{
				return log_message();
			}
			else
			{
				return std::forward<MESSAGE>(log_message);
			}
		}();

		auto GetGlobalLogger = [](auto channel) -> Logger&
		{
			switch (channel)
			{
			case Channel::Certificates:
				return GlobalLogger_Certificates::get();

			case Channel::Coroutines:
				return GlobalLogger_Coroutines::get();

			case Channel::Developer:
				return GlobalLogger_Developer::get();

			case Channel::Devices:
				return GlobalLogger_Devices::get();

			case Channel::Equipment:
				return GlobalLogger_Equipment::get();

			case Channel::Exceptions:
				return GlobalLogger_Exceptions::get();

			case Channel::Main:
				return GlobalLogger_Main::get();

			case Channel::Messages:
				return GlobalLogger_Messages::get();

			case Channel::Mqtt:
				return GlobalLogger_Mqtt::get();

			case Channel::Navigation:
				return GlobalLogger_Navigation::get();

			case Channel::Options:
				return GlobalLogger_Options::get();

			case Channel::Platform:
				return GlobalLogger_Platform::get();

			case Channel::Profiling:
				return GlobalLogger_Profiling::get();

			case Channel::Protocol:
				return GlobalLogger_Protocol::get();

			case Channel::Scraping:
				return GlobalLogger_Scraping::get();

			case Channel::Serial:
				return GlobalLogger_Serial::get();

			case Channel::Signals:
				return GlobalLogger_Signals::get();

			case Channel::Web:
				return GlobalLogger_Web::get();

			default:
				// This is a problem...there's a channel type that has not been
				// added to the above list...default to the Main channel.
				return GlobalLogger_Main::get();
			}
		};

		Logger& lg = GetGlobalLogger(channel);
		const std::string_view source_basename = SourceFileBasename(location.file_name());
		BOOST_LOG_SEV(lg, severity)
			<< boost::log::add_value(source_file, std::string(source_basename))
			<< boost::log::add_value(source_line, location.line())
			<< resolved;

#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
		if (severity >= Severity::Warning)
		{
			uint32_t colour = 0;
			switch (severity)
			{
			case Severity::Warning: colour = static_cast<uint32_t>(Profiling::UnitColours::Orange); break;
			case Severity::Error:   colour = static_cast<uint32_t>(Profiling::UnitColours::Red); break;
			case Severity::Fatal:   colour = static_cast<uint32_t>(Profiling::UnitColours::Magenta); break;
			default: break;
			}

			if (colour != 0)
			{
				if constexpr (std::is_convertible_v<decltype(resolved), std::string_view>)
				{
					Factory::ProfilerFactory::Instance().Get()->Message(std::string_view(resolved), colour);
				}
			}
		}
#endif
	}

}
// namespace AqualinkAutomate::Logging

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogTrace(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Trace, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogDebug(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Debug, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogInfo(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Info, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogNotify(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Notify, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogWarning(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Warning, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogError(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Error, location);
}

template<AqualinkAutomate::Logging::Loggable MESSAGE>
void LogFatal(AqualinkAutomate::Logging::Channel channel, MESSAGE&& log_message, const std::source_location location = std::source_location::current())
{
	AqualinkAutomate::Logging::Log(std::forward<MESSAGE>(log_message), channel, AqualinkAutomate::Logging::Severity::Fatal, location);
}
