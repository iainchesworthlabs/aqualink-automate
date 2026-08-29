#include <cstdint>
#include <string>

#include <boost/test/unit_test.hpp>

#include <boost/program_options.hpp>

#include "options/options_developer_options.h"

using namespace AqualinkAutomate;
namespace po = boost::program_options;

namespace
{
	/// Helper to create a variables_map from simulated command line arguments using
	/// the Developer OptionsProcessor's options description.
	po::variables_map ParseDeveloperOptions(Options::Developer::OptionsProcessor& processor, const std::vector<const char*>& args)
	{
		po::options_description desc;
		desc.add(processor.Options());

		po::variables_map vm;
		po::store(po::parse_command_line(static_cast<int>(args.size()), args.data(), desc), vm);
		po::notify(vm);

		return vm;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_DeveloperOptions)

//-----------------------------------------------------------------------------
// DEFAULT VALUES
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_DefaultStructValues)
{
	// The struct default is the real fallback (Process() only writes present
	// options); replay pacing must default to 15 ms / 1.0x.
	Options::Developer::DeveloperSettings settings;

	BOOST_CHECK_EQUAL(settings.replay_frame_period_ms, 15u);
	BOOST_CHECK_EQUAL(settings.replay_speed, 1.0);
	BOOST_CHECK_EQUAL(settings.dev_mode_enabled, false);
	// On-demand captures keep landing in <cwd>/captures unless the operator
	// moves them, so an existing install is unaffected by the option existing.
	BOOST_CHECK_EQUAL(settings.capture_directory, "captures");
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_ProcessDefaults)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	const auto& settings = result.value();
	BOOST_CHECK_EQUAL(settings.replay_frame_period_ms, 15u);
	BOOST_CHECK_EQUAL(settings.replay_speed, 1.0);
	BOOST_CHECK_EQUAL(settings.capture_directory, "captures");
}

//-----------------------------------------------------------------------------
// CAPTURE DIRECTORY
//
// Where on-demand captures (the diagnostics UI / REST route) are written and
// served from. Configurable so a packaged deployment can put them somewhere the
// user can actually reach - the Home Assistant add-on points it at its
// user-browsable app_config share.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_CaptureDirectory_Custom)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--capture-directory=/config/captures" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().capture_directory, "/config/captures");
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_CaptureDirectory_IsIndependentOfRecordSerial)
{
	// --record-serial is an operator-supplied path used as given; it must NOT be
	// rewritten into (or constrained by) the capture directory.
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--capture-directory=/var/lib/aqualink/captures", "--record-serial=boot.cap" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().capture_directory, "/var/lib/aqualink/captures");
	BOOST_CHECK_EQUAL(result.value().recording_file, "boot.cap");
}

//-----------------------------------------------------------------------------
// REPLAY FRAME PERIOD
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_ReplayFramePeriod_Custom)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--replay-frame-period=30" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().replay_frame_period_ms, 30u);
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_ReplayFramePeriod_ZeroMeansUnpaced)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--replay-frame-period=0" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	// 0 is the documented "unpaced / as fast as possible" sentinel.
	BOOST_CHECK_EQUAL(result.value().replay_frame_period_ms, 0u);
}

//-----------------------------------------------------------------------------
// REPLAY SPEED
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_ReplaySpeed_Custom)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--replay-speed=2.5" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_CLOSE(result.value().replay_speed, 2.5, 0.0001);
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_ReplayPacing_BothTogether)
{
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--replay-frame-period=20", "--replay-speed=0.5" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().replay_frame_period_ms, 20u);
	BOOST_CHECK_CLOSE(result.value().replay_speed, 0.5, 0.0001);
}

//-----------------------------------------------------------------------------
// DATA-DRIVEN PER-CHANNEL LOG LEVELS (enumerated over Logging::Channel)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_LogLevel_KnownChannelParses)
{
	// A representative channel option must still parse and Process cleanly.
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--loglevel-protocol=Trace" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_LogLevel_DeveloperChannelNowExists)
{
	// Regression: the per-channel options are now generated from the Channel
	// enum, so every channel — including Channel::Developer, which had no manual
	// option before — gets a --loglevel-<channel> flag.
	Options::Developer::OptionsProcessor processor;
	auto vm = ParseDeveloperOptions(processor, { "program", "--loglevel-developer=Debug" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
}

BOOST_AUTO_TEST_CASE(Test_DeveloperOptions_LogLevel_EmptyValueDoesNotCrash)
{
	// Regression for the severity-validator out-of-bounds read: an empty value
	// for a log-level option must fail parsing cleanly (it propagates as a
	// boost program_options error from the validator), not read out of bounds.
	Options::Developer::OptionsProcessor processor;
	BOOST_CHECK_THROW(
		(void)ParseDeveloperOptions(processor, { "program", "--loglevel-main", "" }),
		boost::program_options::error);
}

BOOST_AUTO_TEST_SUITE_END()
