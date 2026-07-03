#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "logging/sinks/sink_native.h"
#include "options/options_option_type.h"
#include "options/validators/syslog_facility_validator.h"

// NOTE: this area is namespaced `Options::LogSinks`, NOT `Options::Logging`, so it
// does not collide with the `AqualinkAutomate::Logging` subsystem namespace — main
// does `using namespace AqualinkAutomate::Logging` and refers to `Logging::` a lot,
// which an `Options::Logging` would make ambiguous. The user-facing area name
// (AreaName / --help section) remains "Logging".
namespace AqualinkAutomate::Options::LogSinks
{

	// How the operational sink set is chosen. Auto derives it from the runtime
	// environment (docs/logging-sinks-redesign.md §6.2); Explicit uses the flags below.
	enum class SinkMode
	{
		Auto,
		Explicit
	};

	typedef struct tagLoggingSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Logging" };
			return AREA_NAME;
		}

		tagLoggingSettings() :
			Sinks{ SinkMode::Auto },
			Console{ false },
			Native{ false },
			Facility{ AqualinkAutomate::Logging::Sinks::SyslogFacility::Daemon }
		{
		}

		// Sink selection. When Sinks == Explicit, Console/Native say which operational
		// sinks are active; when Auto they are ignored (the environment decides).
		SinkMode Sinks;
		bool Console;
		bool Native;

		// POSIX syslog facility for the GENERAL native sink (the audit sink always
		// uses LOG_AUTHPRIV regardless — §10.3). Ignored on Windows.
		AqualinkAutomate::Logging::Sinks::SyslogFacility Facility;
	}
	LoggingSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_LOGSINKS{ make_appoption("log-sinks", "Which log sinks to use: 'auto' (environment-derived) or a comma-separated list of 'console','native'", boost::program_options::value<std::string>()->default_value("auto")) };
		AppOptionPtr OPTION_LOGFACILITY{ make_appoption("log-syslog-facility", "POSIX syslog facility for the general native sink: daemon, user, local0-local7", boost::program_options::value<AqualinkAutomate::Logging::Sinks::SyslogFacility>()->default_value(AqualinkAutomate::Logging::Sinks::SyslogFacility::Daemon, "daemon")) };

		const std::vector<AppOptionPtr> LoggingOptionsCollection
		{
			OPTION_LOGSINKS,
			OPTION_LOGFACILITY
		};

	public:
		using SettingsType = LoggingSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::LogSinks
