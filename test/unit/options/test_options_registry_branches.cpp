#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/program_options.hpp>

#include "errors/options_errors.h"
#include "exceptions/exception_optionshelporversion.h"
#include "options/options.h"

using namespace AqualinkAutomate;
namespace po = boost::program_options;

//=============================================================================
// The options pipeline's Process() fold is a SHORT-CIRCUIT: the first
// processor whose Process() reports an error aborts the whole run, and no
// later processor is invoked.  This matters because a half-populated Settings
// (some areas present, the failing one missing) previously surfaced only as a
// mystery Get() failure far downstream.
//
// Also covered here: --version-detail, the detailed-version arm of
// HandleVersion (the plain --version arm is already exercised elsewhere).
//=============================================================================

namespace
{

	// A processor whose Process() always fails, tagged so the test can tell two
	// failing processors apart by the error they report.
	template<AqualinkAutomate::ErrorCodes::Options_ErrorCodes ERROR_CODE, const char* AREA_NAME>
	struct TaggedFailingProcessor
	{
		struct SettingsType
		{
			static const std::string& AreaName()
			{
				static const std::string name{ AREA_NAME };
				return name;
			}
		};

		std::string Name() const { return SettingsType::AreaName(); }

		po::options_description Options() const
		{
			return po::options_description{ SettingsType::AreaName() };
		}

		void Validate(const po::variables_map&) const {}

		std::expected<SettingsType, AqualinkAutomate::ErrorCodes::Options_ErrorCodes> Process(po::variables_map&) const
		{
			return std::unexpected(ERROR_CODE);
		}
	};

	// A processor that succeeds and records that its Process() actually ran, so
	// the test can prove the fold skipped it after an earlier failure.
	inline bool g_recording_processor_ran{ false };

	struct RecordingProcessor
	{
		struct SettingsType
		{
			static const std::string& AreaName()
			{
				static const std::string name{ "RecordingArea" };
				return name;
			}
		};

		std::string Name() const { return SettingsType::AreaName(); }

		po::options_description Options() const
		{
			return po::options_description{ SettingsType::AreaName() };
		}

		void Validate(const po::variables_map&) const {}

		std::expected<SettingsType, AqualinkAutomate::ErrorCodes::Options_ErrorCodes> Process(po::variables_map&) const
		{
			g_recording_processor_ran = true;
			return SettingsType{};
		}
	};

	inline constexpr char FIRST_AREA[] = "FirstFailingArea";
	inline constexpr char SECOND_AREA[] = "SecondFailingArea";

	using FirstFailing = TaggedFailingProcessor<AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsHandlingFailed, FIRST_AREA>;
	using SecondFailing = TaggedFailingProcessor<AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsValidationFailed, SECOND_AREA>;

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_OptionsRegistryBranches)

// A failure in the first processor must stop the fold: the later processor's
// Process() is never called.
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_FailureSkipsLaterProcessors)
{
	g_recording_processor_ran = false;

	const char* argv[] = { "program" };
	const int argc = 1;

	auto result = Options::Initialise()
		| Options::Add(FirstFailing{})
		| Options::Add(RecordingProcessor{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(FirstFailing{}, RecordingProcessor{})
		| Options::Finalise();

	BOOST_REQUIRE(!result.has_value());
	BOOST_CHECK(AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsHandlingFailed == result.error());
	BOOST_CHECK_MESSAGE(!g_recording_processor_ran, "the fold must not run a processor after an earlier failure");
}

// A processor BEFORE the failing one still runs (the short-circuit starts at
// the failure, it does not skip the whole fold).
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_ProcessorsBeforeTheFailureStillRun)
{
	g_recording_processor_ran = false;

	const char* argv[] = { "program" };
	const int argc = 1;

	auto result = Options::Initialise()
		| Options::Add(RecordingProcessor{})
		| Options::Add(FirstFailing{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(RecordingProcessor{}, FirstFailing{})
		| Options::Finalise();

	BOOST_REQUIRE(!result.has_value());
	BOOST_CHECK(AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsHandlingFailed == result.error());
	BOOST_CHECK_MESSAGE(g_recording_processor_ran, "a processor ahead of the failure must still have run");
}

// With two failing processors the FIRST error is the one reported - later
// failures cannot overwrite it.
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_FirstErrorWins)
{
	const char* argv[] = { "program" };
	const int argc = 1;

	auto result = Options::Initialise()
		| Options::Add(FirstFailing{})
		| Options::Add(SecondFailing{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(FirstFailing{}, SecondFailing{})
		| Options::Finalise();

	BOOST_REQUIRE(!result.has_value());
	BOOST_CHECK(AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsHandlingFailed == result.error());

	// Reversing the order reverses which error surfaces, proving it really is
	// the FIRST failure that is reported rather than a fixed value.
	auto reversed = Options::Initialise()
		| Options::Add(SecondFailing{})
		| Options::Add(FirstFailing{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(SecondFailing{}, FirstFailing{})
		| Options::Finalise();

	BOOST_REQUIRE(!reversed.has_value());
	BOOST_CHECK(AqualinkAutomate::ErrorCodes::Options_ErrorCodes::OptionsValidationFailed == reversed.error());
}

// The whole fold succeeding populates every area (the "no error at any step"
// path through Process_RunOne).
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_AllProcessorsSucceedPopulatesAreas)
{
	g_recording_processor_ran = false;

	const char* argv[] = { "program" };
	const int argc = 1;

	auto result = Options::Initialise()
		| Options::Add(RecordingProcessor{})
		| Options::Parse(argc, const_cast<char**>(argv))
		| Options::Notify()
		| Options::Validate()
		| Options::Process(RecordingProcessor{})
		| Options::Finalise();

	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(g_recording_processor_ran);
	BOOST_CHECK(result.value().Has(RecordingProcessor::SettingsType::AreaName()));
}

//=============================================================================
// --version-detail
//=============================================================================

// The detailed-version request short-circuits the pipeline exactly like
// --version does (writing the build/commit details to stdout and throwing
// OptionsHelpOrVersion), even alongside options that would fail validation.
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_VersionDetailShortCircuits)
{
	const std::vector<const char*> args{ "program", "--version-detail", "--disable-https", "--https-port=8443" };

	auto run = [&args]()
		{
			return Options::Initialise()
				| Options::Add(Options::App::OptionsProcessor{})
				| Options::Add(Options::Web::OptionsProcessor{})
				| Options::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data()))
				| Options::CheckHelpAndVersion()
				| Options::Validate()
				| Options::Process(Options::App::OptionsProcessor{}, Options::Web::OptionsProcessor{})
				| Options::Finalise();
		};

	BOOST_CHECK_THROW((void)run(), Exceptions::OptionsHelpOrVersion);
}

// --version-detail takes precedence over a plain --version offered at the same
// time (the detailed output is the more specific request).
BOOST_AUTO_TEST_CASE(OptionsRegistryBranches_VersionDetailWithPlainVersionStillShortCircuits)
{
	const std::vector<const char*> args{ "program", "--version", "--version-detail" };

	auto run = [&args]()
		{
			return Options::Initialise()
				| Options::Add(Options::App::OptionsProcessor{})
				| Options::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data()))
				| Options::CheckHelpAndVersion()
				| Options::Validate()
				| Options::Process(Options::App::OptionsProcessor{})
				| Options::Finalise();
		};

	BOOST_CHECK_THROW((void)run(), Exceptions::OptionsHelpOrVersion);
}

BOOST_AUTO_TEST_SUITE_END()
