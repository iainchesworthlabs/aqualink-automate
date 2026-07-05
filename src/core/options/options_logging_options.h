#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "logging/logging_formatter.h"
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

		tagLoggingSettings() = default;

		// Sink selection. When Sinks == Explicit, Console/Native/File say which
		// operational sinks are active; when Auto they are ignored (the environment
		// decides, though a --log-file always implies the file sink).
		SinkMode Sinks{ SinkMode::Auto };
		bool Console{ false };
		bool Native{ false };
		bool File{ false };
		bool Journald{ false };  // native journald sink (Linux/systemd); falls back to console+prefixes elsewhere

		// POSIX syslog facility for the GENERAL native sink (the audit sink always
		// uses LOG_AUTHPRIV regardless — §10.3). Ignored on Windows.
		AqualinkAutomate::Logging::Sinks::SyslogFacility Facility{ AqualinkAutomate::Logging::Sinks::SyslogFacility::Daemon };

		// Wire format for the console + file sinks.
		AqualinkAutomate::Logging::LogFormat Format{ AqualinkAutomate::Logging::LogFormat::Text };

		// File sink target + rotation bounds. LogFile unset => no file sink.
		std::optional<std::filesystem::path> LogFile;
		std::uintmax_t LogFileMaxBytes{ 10ULL * 1024ULL * 1024ULL };
		std::size_t LogFileMaxFiles{ 5 };
	}
	LoggingSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_LOGSINKS{ make_appoption("log-sinks", "Which log sinks to use: 'auto' (environment-derived) or a comma-separated list of 'console','native','file','journald' ('journald' is Linux/systemd only; elsewhere it falls back to console with priority prefixes)", boost::program_options::value<std::string>()->default_value("auto")) };
		AppOptionPtr OPTION_LOGFACILITY{ make_appoption("log-syslog-facility", "POSIX syslog facility for the general native sink: daemon, user, local0-local7", boost::program_options::value<AqualinkAutomate::Logging::Sinks::SyslogFacility>()->default_value(AqualinkAutomate::Logging::Sinks::SyslogFacility::Daemon, "daemon")) };
		AppOptionPtr OPTION_LOGFORMAT{ make_appoption("log-format", "Log record format for the console and file sinks: 'text' or 'json'", boost::program_options::value<std::string>()->default_value("text")) };
		AppOptionPtr OPTION_LOGFILE{ make_appoption("log-file", "Write logs to this file (enables the file sink; rotated + size-bounded)", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_LOGFILEMAXSIZE{ make_appoption("log-file-max-size", "Rotate the log file when it would exceed this many bytes", boost::program_options::value<std::uintmax_t>()->default_value(10ULL * 1024ULL * 1024ULL)) };
		AppOptionPtr OPTION_LOGFILEMAXFILES{ make_appoption("log-file-max-files", "Keep at most this many rotated log files", boost::program_options::value<std::uint32_t>()->default_value(5)) };

		const std::vector<AppOptionPtr> LoggingOptionsCollection
		{
			OPTION_LOGSINKS,
			OPTION_LOGFACILITY,
			OPTION_LOGFORMAT,
			OPTION_LOGFILE,
			OPTION_LOGFILEMAXSIZE,
			OPTION_LOGFILEMAXFILES
		};

	public:
		using SettingsType = LoggingSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::LogSinks
