#include <boost/test/unit_test.hpp>

#include <vector>

#include <boost/program_options.hpp>

#include "options/options_config_file.h"
#include "options/options_logging_options.h"
#include "options/options_registry.h"

using namespace AqualinkAutomate;
namespace po = boost::program_options;

namespace
{
	po::variables_map ParseLoggingOptions(Options::LogSinks::OptionsProcessor& processor, const std::vector<const char*>& args)
	{
		po::options_description desc;
		desc.add(processor.Options());

		po::variables_map vm;
		po::store(po::parse_command_line(static_cast<int>(args.size()), args.data(), desc), vm);
		po::notify(vm);

		return vm;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_LoggingOptions)

BOOST_AUTO_TEST_CASE(Defaults_AutoModeAndDaemonFacility)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK(result->Sinks == Options::LogSinks::SinkMode::Auto);
	BOOST_CHECK(result->Facility == Logging::Sinks::SyslogFacility::Daemon);
}

BOOST_AUTO_TEST_CASE(ExplicitConsoleOnly)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", "console" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK(result->Sinks == Options::LogSinks::SinkMode::Explicit);
	BOOST_CHECK(result->Console);
	BOOST_CHECK(!result->Native);
}

BOOST_AUTO_TEST_CASE(ExplicitConsoleAndNative_CaseAndSpaceInsensitive)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", " Console , NATIVE " });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK(result->Console);
	BOOST_CHECK(result->Native);
}

BOOST_AUTO_TEST_CASE(ExplicitNativeOnly)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", "native" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK(result->Sinks == Options::LogSinks::SinkMode::Explicit);
	BOOST_CHECK(!result->Console);
	BOOST_CHECK(result->Native);
}

BOOST_AUTO_TEST_CASE(AutoKeyword_ResolvesToAutoMode)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", "auto" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(result->Sinks == Options::LogSinks::SinkMode::Auto);
}

BOOST_AUTO_TEST_CASE(UnknownSinkToken_FailsValidation)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", "console,bogus" });

	// 'bogus' (and 'file', until the file-sink slice) is not an accepted token.
	auto result = processor.Process(vm);
	BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(EmptyExplicitSet_FailsValidation)
{
	Options::LogSinks::OptionsProcessor processor;
	auto vm = ParseLoggingOptions(processor, { "program", "--log-sinks", "" });

	auto result = processor.Process(vm);
	BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(Facility_ParsesLocalAndAuthPrivCaseInsensitive)
{
	{
		Options::LogSinks::OptionsProcessor processor;
		auto vm = ParseLoggingOptions(processor, { "program", "--log-syslog-facility", "local3" });
		auto result = processor.Process(vm);
		BOOST_REQUIRE(result.has_value());
		BOOST_CHECK(result->Facility == Logging::Sinks::SyslogFacility::Local3);
	}
	{
		Options::LogSinks::OptionsProcessor processor;
		auto vm = ParseLoggingOptions(processor, { "program", "--log-syslog-facility", "AUTHPRIV" });
		auto result = processor.Process(vm);
		BOOST_REQUIRE(result.has_value());
		BOOST_CHECK(result->Facility == Logging::Sinks::SyslogFacility::AuthPriv);
	}
}

BOOST_AUTO_TEST_CASE(FullPipeline_InvalidFacilityFailsValidation)
{
	const char* argv[] = { "program", "--log-syslog-facility", "not-a-facility" };
	int argc = 3;

	auto processed_options = Options::Initialise()
		| Options::Add(Options::LogSinks::OptionsProcessor{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(Options::LogSinks::OptionsProcessor{})
		| Options::Finalise();

	BOOST_CHECK(!processed_options.has_value());
}

BOOST_AUTO_TEST_CASE(FullPipeline_DefaultsResolveToAuto)
{
	const char* argv[] = { "program" };
	int argc = 1;

	auto processed_options = Options::Initialise()
		| Options::Add(Options::LogSinks::OptionsProcessor{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(Options::LogSinks::OptionsProcessor{})
		| Options::Finalise();

	BOOST_REQUIRE(processed_options.has_value());

	auto logging_settings = processed_options.value().Get<Options::LogSinks::LoggingSettings>();
	BOOST_REQUIRE(logging_settings.has_value());
	BOOST_CHECK(logging_settings.value().get().Sinks == Options::LogSinks::SinkMode::Auto);
}

BOOST_AUTO_TEST_SUITE_END()
