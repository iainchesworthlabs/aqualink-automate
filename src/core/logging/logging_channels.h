#pragma once

namespace AqualinkAutomate::Logging
{
	enum class Channel
	{
		// NOTE: there is deliberately no `Audit` channel. The security audit trail is
		// a separate subsystem, not an operational log channel (see
		// docs/logging-sinks-redesign.md §10 and the `is_audit` attribute in
		// logging_attributes.h). Do not add it back here.
		Certificates,
		Coroutines,
		Developer,
		Devices,
		Equipment,
		Exceptions,
		Main,
		Messages,
		Mqtt,
		Navigation,
		Options,
		Platform,
		Profiling,
		Protocol,
		Scraping,
		Serial,
		Signals,
		Web
	};
}
// namespace AqualinkAutomate::Logging
