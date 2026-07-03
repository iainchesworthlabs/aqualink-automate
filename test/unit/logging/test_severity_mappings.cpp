#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <string>

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>

#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"

//
// Exhaustive coverage for the OS-native severity mappings (docs/logging-sinks-redesign.md §7).
//
// The critical property: EVERY Logging::Severity maps to an explicit, intended
// syslog level / Windows event type / journald priority. On MSVC an unhandled
// switch case is not a build error, so a new Severity enumerator would silently
// take the trailing fallback. These tests are the cross-platform backstop: the
// count assertion below fails the build if the enum grows without this table
// being updated, and enum_for_each then pins every value.
//

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Logging::Sinks;

namespace
{
	struct ExpectedMapping
	{
		SyslogLevel Level;
		EventType   Event;
		int         Priority;   // journald "<N>"
	};

	// The §7 table, expressed independently of the production code so the test is a
	// genuine oracle rather than a mirror of the implementation.
	[[nodiscard]] ExpectedMapping Expected(Severity severity)
	{
		switch (severity)
		{
		case Severity::Trace:   return { SyslogLevel::Debug,    EventType::Information, 7 };
		case Severity::Debug:   return { SyslogLevel::Debug,    EventType::Information, 7 };
		case Severity::Info:    return { SyslogLevel::Info,     EventType::Information, 6 };
		case Severity::Notify:  return { SyslogLevel::Notice,   EventType::Information, 5 };
		case Severity::Warning: return { SyslogLevel::Warning,  EventType::Warning,     4 };
		case Severity::Error:   return { SyslogLevel::Error,    EventType::Error,       3 };
		case Severity::Fatal:   return { SyslogLevel::Critical, EventType::Error,       2 };
		}

		BOOST_FAIL("Severity value not present in the test's expected table");
		return { SyslogLevel::Info, EventType::Information, 6 };
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_SeverityMappings)

// Build-time guard: if the Severity enum gains a value, this fails until both the
// production table and the Expected() oracle above are updated.
BOOST_AUTO_TEST_CASE(EnumHasSevenSeverities, *boost::unit_test::label("unit"))
{
	BOOST_TEST(magic_enum::enum_count<Severity>() == static_cast<std::size_t>(7));
}

BOOST_AUTO_TEST_CASE(EverySeverityMapsAsSpecified, *boost::unit_test::label("unit"))
{
	magic_enum::enum_for_each<Severity>([](Severity severity)
		{
			const auto expected = Expected(severity);

			BOOST_TEST_INFO("severity = " << magic_enum::enum_name(severity));

			BOOST_TEST(static_cast<int>(ToSyslogLevel(severity)) == static_cast<int>(expected.Level));
			BOOST_TEST(static_cast<int>(ToEventType(severity)) == static_cast<int>(expected.Event));
			BOOST_TEST(SyslogPriorityValue(severity) == expected.Priority);
		});
}

// The journald "<N>" prefix is exactly "<" + priority + ">".
BOOST_AUTO_TEST_CASE(JournaldPrefixIsAngleBracketedPriority, *boost::unit_test::label("unit"))
{
	magic_enum::enum_for_each<Severity>([](Severity severity)
		{
			const std::string expected = "<" + std::to_string(SyslogPriorityValue(severity)) + ">";

			BOOST_TEST_INFO("severity = " << magic_enum::enum_name(severity));
			BOOST_TEST(JournaldPrefix(severity) == expected);
		});
}

// Spot-check the two that matter most for operational monitoring: an Error must
// be syslog priority 3 (err) so `journalctl -p err` finds it, and a Warning 4.
BOOST_AUTO_TEST_CASE(ErrorAndWarningCarryDistinctPriorities, *boost::unit_test::label("unit"))
{
	BOOST_TEST(SyslogPriorityValue(Severity::Error) == 3);
	BOOST_TEST(SyslogPriorityValue(Severity::Warning) == 4);
	BOOST_TEST(SyslogPriorityValue(Severity::Error) != SyslogPriorityValue(Severity::Info));
}

BOOST_AUTO_TEST_SUITE_END()
