#pragma once

#include <cstdint>
#include <string>

#include <boost/log/attributes.hpp>
#include <boost/log/expressions/keyword.hpp>

#include "logging/logging_channels.h"
#include "logging/logging_severity_levels.h"

BOOST_LOG_ATTRIBUTE_KEYWORD(line_id, "LineID", uint32_t)
BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", AqualinkAutomate::Logging::Severity)
BOOST_LOG_ATTRIBUTE_KEYWORD(channel, "Channel", AqualinkAutomate::Logging::Channel)
BOOST_LOG_ATTRIBUTE_KEYWORD(source_line, "Line", uint32_t)
BOOST_LOG_ATTRIBUTE_KEYWORD(source_file, "File", std::string)

// Marks a record as belonging to the security audit trail rather than the
// operational logs. Audit is NOT a Logging::Channel (docs/logging-sinks-redesign.md
// §10): audit records carry this attribute, the audit native sink accepts only
// records that carry it, and every operational sink is built through the shared
// filter that excludes it (see sinks/sink_filters.h).
BOOST_LOG_ATTRIBUTE_KEYWORD(is_audit, "IsAudit", bool)
