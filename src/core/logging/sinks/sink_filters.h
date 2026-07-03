#pragma once

#include <boost/log/expressions/filter.hpp>

namespace AqualinkAutomate::Logging::Sinks
{

	//
	// The single filter every OPERATIONAL sink (console, file, general native) is
	// built with: it excludes audit records (`is_audit`) and applies the per-channel
	// severity gate to everything else. Routing all operational sinks through this
	// one helper is what keeps the audit trail from leaking onto an operational sink
	// (docs/logging-sinks-redesign.md §10.2) — the exclusion holds because the sink
	// is constructed through here, not because each call site remembers it.
	//
	[[nodiscard]] boost::log::filter MakeOperationalFilter();

	//
	// The filter for the AUDIT sink: accepts ONLY audit records. Audit is not a
	// Logging::Channel, so there is no severity gate — audit events are always
	// recorded when the audit sink is installed.
	//
	[[nodiscard]] boost::log::filter MakeAuditFilter();

}
// namespace AqualinkAutomate::Logging::Sinks
