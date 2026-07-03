#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/core.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include "logging/logging.h"
#include "logging/logging_initialise.h"
#include "logging/sinks/log_environment.h"
#include "logging/sinks/sink_console.h"
#include "logging/sinks/sink_file.h"
#include "logging/sinks/sink_filters.h"
#include "logging/sinks/sink_journald.h"
#include "logging/sinks/sink_native.h"
#include "logging/sinks/sink_registry.h"

namespace AqualinkAutomate::Logging
{

void Initialise(LogFormat format)
{
	// Bootstrap console sink, installed via the registry before options are parsed
	// so start-up diagnostics are visible. This resolves the `auto` console arm:
	// text (or JSON, if main's argv pre-scan detected --log-format json), plus
	// sd-daemon "<N>" priority prefixes when stderr is connected to the journal
	// (StandardOutput=journal under systemd). The native/file sinks and any
	// --log-sinks overrides are applied after options are processed (the reconfigure
	// step); the console format is finalised there too.
	Sinks::ConsoleSinkConfig console_config;
	console_config.JournaldPrefixes = Sinks::DetectLogEnvironment().StderrIsJournal;
	console_config.Format = format;

	Sinks::SinkRegistry::Add(Sinks::MakeConsoleSink(console_config));

	boost::log::add_common_attributes();
	boost::log::core::get()->add_global_attribute("Scope", boost::log::attributes::named_scope());
}

void Reconfigure(const RuntimeConfig& config)
{
	// Drop the bootstrap console and install the resolved operational sink set.
	// (The audit sink is installed separately by the auth bootstrap and is not
	// tracked here, so it is untouched.)
	Sinks::SinkRegistry::RemoveAll();

	if (config.Selection.Console)
	{
		Sinks::ConsoleSinkConfig console_config;
		console_config.JournaldPrefixes = config.Selection.ConsoleJournaldPrefixes;
		console_config.Format = config.Format;
		Sinks::SinkRegistry::Add(Sinks::MakeConsoleSink(console_config));
	}

	if (config.Selection.Native)
	{
		Sinks::SinkRegistry::Add(Sinks::MakeNativeSink(Sinks::NativeSinkConfig{
			.Filter = Sinks::MakeOperationalFilter(),
			.Facility = config.GeneralNativeFacility }));
	}

	if (config.Selection.Journald)
	{
		// MakeJournaldSink returns null on non-Linux (stub) or when libsystemd is
		// absent, so no platform #ifdef is needed here.
		auto journald_sink = Sinks::MakeJournaldSink(Sinks::JournaldSinkConfig{ .Filter = Sinks::MakeOperationalFilter() });

		if (journald_sink)
		{
			Sinks::SinkRegistry::Add(journald_sink);
		}
		else
		{
			// journald was requested (e.g. --log-sinks journald) but is not available
			// (non-Linux, or libsystemd absent): fall back to the console with priority
			// prefixes so priorities still reach the journal, and say so.
			LogWarning(Channel::Main, "journald sink unavailable (no libsystemd); using console with priority prefixes instead");

			Sinks::ConsoleSinkConfig fallback_console;
			fallback_console.JournaldPrefixes = true;
			fallback_console.Format = config.Format;
			Sinks::SinkRegistry::Add(Sinks::MakeConsoleSink(fallback_console));
		}
	}

	if (config.Selection.File && config.LogFilePath.has_value())
	{
		Sinks::SinkRegistry::Add(Sinks::MakeFileSink(Sinks::FileSinkConfig{
			.Path = *config.LogFilePath,
			.MaxFileBytes = config.LogFileMaxBytes,
			.MaxFiles = config.LogFileMaxFiles,
			.Format = config.Format }));
	}
}

void Shutdown()
{
	Sinks::SinkRegistry::FlushAll();
	Sinks::SinkRegistry::RemoveAll();
}

}
// namespace AqualinkAutomate::Logging
