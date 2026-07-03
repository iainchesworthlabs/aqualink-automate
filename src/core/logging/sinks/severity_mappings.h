#pragma once

#include <string>

#include "logging/logging_severity_levels.h"

namespace AqualinkAutomate::Logging::Sinks
{

	//
	// Platform-independent severity mappings shared by every OS-native sink and by
	// the journald "<N>" priority prefix. Kept free of any Boost.Log sink header so
	// it compiles and is exhaustively testable on every platform (the Windows arm
	// of the native sink is #if-gated, but this table is not).
	//
	// The numeric values of SyslogLevel deliberately match RFC 5424 / the POSIX
	// syslog(3) priorities AND Boost's boost::log::sinks::syslog::level enum, so the
	// POSIX native sink can translate with a plain static_cast and journald can use
	// the integer directly as the "<N>" prefix.
	//

	enum class SyslogLevel : int
	{
		Emergency = 0,
		Alert     = 1,
		Critical  = 2,
		Error     = 3,
		Warning   = 4,
		Notice    = 5,
		Info      = 6,
		Debug     = 7
	};

	//
	// The Windows Event Log record classification. An abstraction over
	// boost::log::sinks::event_log::event_type (whose header is Windows-only); the
	// Windows native sink maps these three onto that enum. Nothing here maps to the
	// Success/AuditSuccess/AuditFailure Event Log types — application severities do
	// not carry that meaning.
	//
	enum class EventType
	{
		Information,
		Warning,
		Error
	};

	//
	// Severity -> Syslog level (docs/logging-sinks-redesign.md §7). Notify finally
	// gets its natural syslog analogue (notice); nothing maps to emergency/alert
	// (system-wide conditions, not an application's to raise) and Fatal maps to
	// critical rather than emergency.
	//
	// NOTE: no `default:` label is intentional. On GCC/Clang -Wswitch (warnings are
	// errors project-wide) turns a newly added Severity enumerator into a BUILD
	// failure here; the trailing return then supplies defined behaviour for any
	// value outside the declared enumerators. MSVC does not flag the missing case,
	// so the exhaustive magic_enum unit test is the cross-platform backstop.
	//
	[[nodiscard]] constexpr SyslogLevel ToSyslogLevel(Severity severity) noexcept
	{
		switch (severity)
		{
		case Severity::Trace:   return SyslogLevel::Debug;
		case Severity::Debug:   return SyslogLevel::Debug;
		case Severity::Info:    return SyslogLevel::Info;
		case Severity::Notify:  return SyslogLevel::Notice;
		case Severity::Warning: return SyslogLevel::Warning;
		case Severity::Error:   return SyslogLevel::Error;
		case Severity::Fatal:   return SyslogLevel::Critical;
		}

		return SyslogLevel::Info;
	}

	//
	// Severity -> Windows Event Log record type (docs/logging-sinks-redesign.md §7).
	// Same no-default rationale as ToSyslogLevel().
	//
	[[nodiscard]] constexpr EventType ToEventType(Severity severity) noexcept
	{
		switch (severity)
		{
		case Severity::Trace:   return EventType::Information;
		case Severity::Debug:   return EventType::Information;
		case Severity::Info:    return EventType::Information;
		case Severity::Notify:  return EventType::Information;
		case Severity::Warning: return EventType::Warning;
		case Severity::Error:   return EventType::Error;
		case Severity::Fatal:   return EventType::Error;
		}

		return EventType::Information;
	}

	//
	// The integer syslog priority (0..7) for a record's severity. This is the value
	// journald reads from an sd-daemon "<N>" line prefix on a StandardError=journal
	// stream to recover the real priority.
	//
	[[nodiscard]] constexpr int SyslogPriorityValue(Severity severity) noexcept
	{
		return static_cast<int>(ToSyslogLevel(severity));
	}

	//
	// The sd-daemon priority prefix ("<4>" for a Warning, etc.) prepended to a
	// console line when stderr is connected to the journal. Defined out-of-line so
	// the header stays free of <format>/allocation concerns for its constexpr core.
	//
	[[nodiscard]] std::string JournaldPrefix(Severity severity);

}
// namespace AqualinkAutomate::Logging::Sinks
