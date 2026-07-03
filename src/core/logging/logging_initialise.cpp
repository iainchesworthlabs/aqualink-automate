#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/core.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include "logging/logging_initialise.h"
#include "logging/sinks/log_environment.h"
#include "logging/sinks/sink_console.h"
#include "logging/sinks/sink_filters.h"
#include "logging/sinks/sink_native.h"
#include "logging/sinks/sink_registry.h"

namespace AqualinkAutomate::Logging
{

void Initialise()
{
	// Bootstrap console sink, installed via the registry before options are parsed
	// so start-up diagnostics are visible. This resolves the `auto` console arm:
	// plain text, plus sd-daemon "<N>" priority prefixes when stderr is connected to
	// the journal (StandardOutput=journal under systemd). The native/file sinks and
	// any --log-format/--log-sinks overrides are applied after options are processed
	// (see the post-options reconfigure step).
	Sinks::ConsoleSinkConfig console_config;
	console_config.JournaldPrefixes = Sinks::DetectLogEnvironment().StderrIsJournal;

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
		Sinks::SinkRegistry::Add(Sinks::MakeConsoleSink(console_config));
	}

	if (config.Selection.Native)
	{
		Sinks::SinkRegistry::Add(Sinks::MakeNativeSink(Sinks::NativeSinkConfig{
			.Filter = Sinks::MakeOperationalFilter(),
			.Facility = config.GeneralNativeFacility }));
	}

	// The file sink is added by a later slice (it needs the --log-file option).
}

void Shutdown()
{
	Sinks::SinkRegistry::FlushAll();
	Sinks::SinkRegistry::RemoveAll();
}

}
// namespace AqualinkAutomate::Logging
