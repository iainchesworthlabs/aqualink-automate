#include <boost/test/unit_test.hpp>

#include <sstream>
#include <string>

#include <boost/log/core.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_console.h"
#include "logging/sinks/sink_registry.h"

//
// Behavioural coverage for the console sink and the sink registry
// (docs/logging-sinks-redesign.md §5.1, §5.2). Output is captured through an
// injected std::ostringstream, so these assert the real Boost.Log emit path
// end-to-end without writing to the terminal.
//

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Logging::Sinks;

namespace
{
	//
	// Isolates the global Boost.Log core: starts each case with the common
	// attributes present (LineID etc., needed by the formatter) and no sinks, and
	// restores that clean state plus the default filter afterwards so ordering
	// cannot leak between suites.
	//
	struct CoreSinkFixture
	{
		CoreSinkFixture()
		{
			boost::log::add_common_attributes();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(Severity::Trace);
		}

		~CoreSinkFixture()
		{
			SinkRegistry::RemoveAll();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(SeverityFiltering::DEFAULT_SEVERITY);
		}

		boost::shared_ptr<std::ostringstream> Capture = boost::make_shared<std::ostringstream>();
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_SinkConsoleAndRegistry)

BOOST_FIXTURE_TEST_CASE(ConsoleSink_RendersChannelAndMessage, CoreSinkFixture, *boost::unit_test::label("unit"))
{
	ConsoleSinkConfig config;
	config.Stream = Capture;
	SinkRegistry::Add(MakeConsoleSink(config));

	LogWarning(Channel::Web, "hello-from-web");
	SinkRegistry::FlushAll();

	const std::string out = Capture->str();
	BOOST_TEST(out.find("hello-from-web") != std::string::npos);
	BOOST_TEST(out.find("Web") != std::string::npos);        // channel name rendered
	BOOST_TEST(out.find("Warning") != std::string::npos);    // severity rendered

	// Text format (no journald prefixes) must NOT start with an "<N>" prefix.
	BOOST_TEST(out.rfind("<", 0) != static_cast<std::size_t>(0));
}

BOOST_FIXTURE_TEST_CASE(ConsoleSink_JournaldPrefix_PrependsSyslogPriority, CoreSinkFixture, *boost::unit_test::label("unit"))
{
	ConsoleSinkConfig config;
	config.Stream = Capture;
	config.JournaldPrefixes = true;
	SinkRegistry::Add(MakeConsoleSink(config));

	// Warning maps to syslog priority 4 (§7), so the line must begin with "<4>".
	LogWarning(Channel::Main, "warn-line");
	SinkRegistry::FlushAll();

	const std::string out = Capture->str();
	BOOST_TEST(out.rfind("<4>", 0) == static_cast<std::size_t>(0));
	BOOST_TEST(out.find("warn-line") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(ConsoleSink_JournaldPrefix_ErrorIsPriorityThree, CoreSinkFixture, *boost::unit_test::label("unit"))
{
	ConsoleSinkConfig config;
	config.Stream = Capture;
	config.JournaldPrefixes = true;
	SinkRegistry::Add(MakeConsoleSink(config));

	LogError(Channel::Main, "err-line");
	SinkRegistry::FlushAll();

	BOOST_TEST(Capture->str().rfind("<3>", 0) == static_cast<std::size_t>(0));
}

BOOST_FIXTURE_TEST_CASE(Registry_TracksCount_AndAddDelivers, CoreSinkFixture, *boost::unit_test::label("unit"))
{
	BOOST_TEST(SinkRegistry::Count() == static_cast<std::size_t>(0));

	ConsoleSinkConfig config;
	config.Stream = Capture;
	SinkRegistry::Add(MakeConsoleSink(config));

	BOOST_TEST(SinkRegistry::Count() == static_cast<std::size_t>(1));

	LogInfo(Channel::Main, "delivered");
	SinkRegistry::FlushAll();
	BOOST_TEST(Capture->str().find("delivered") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(Registry_RemoveAll_StopsDelivery, CoreSinkFixture, *boost::unit_test::label("unit"))
{
	ConsoleSinkConfig config;
	config.Stream = Capture;
	SinkRegistry::Add(MakeConsoleSink(config));

	LogInfo(Channel::Main, "before-remove");
	SinkRegistry::FlushAll();
	const auto size_before = Capture->str().size();
	BOOST_TEST(size_before > static_cast<std::size_t>(0));

	SinkRegistry::RemoveAll();
	BOOST_TEST(SinkRegistry::Count() == static_cast<std::size_t>(0));

	// With the sink removed from the core, nothing further reaches the stream.
	LogInfo(Channel::Main, "after-remove");
	BOOST_TEST(Capture->str().size() == size_before);
	BOOST_TEST(Capture->str().find("after-remove") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
