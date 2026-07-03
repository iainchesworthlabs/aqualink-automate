#include <boost/test/unit_test.hpp>

#include <string>
#include <tuple>
#include <vector>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include "exceptions/exception_optionparsingfailed.h"
#include "options/options_auth_options.h"

using namespace AqualinkAutomate;

namespace
{
	boost::program_options::variables_map ParseArgs(const Options::Auth::OptionsProcessor& processor, const std::vector<std::string>& args)
	{
		boost::program_options::variables_map vm;
		boost::program_options::store(boost::program_options::command_line_parser(args).options(processor.Options()).run(), vm);
		boost::program_options::notify(vm);
		return vm;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_OptionsAuth)

BOOST_AUTO_TEST_CASE(Test_OptionsAuth_DefaultsPreserveHistoricalBehaviour)
{
	Options::Auth::OptionsProcessor processor;
	auto vm = ParseArgs(processor, {});

	processor.Validate(vm);
	const auto settings = processor.Process(vm);

	BOOST_REQUIRE(settings.has_value());
	BOOST_CHECK(!settings->auth_mode_enabled);      // Off by default — no behaviour change.
	BOOST_CHECK(settings->auth_state_dir.empty());
	BOOST_CHECK_EQUAL(settings->jwt_access_ttl_minutes, 15u);
}

BOOST_AUTO_TEST_CASE(Test_OptionsAuth_EnabledMode)
{
	Options::Auth::OptionsProcessor processor;
	auto vm = ParseArgs(processor, { "--auth-mode", "enabled", "--auth-state-dir", "C:/state/auth", "--jwt-access-ttl", "30" });

	processor.Validate(vm);
	const auto settings = processor.Process(vm);

	BOOST_REQUIRE(settings.has_value());
	BOOST_CHECK(settings->auth_mode_enabled);
	BOOST_CHECK_EQUAL(settings->auth_state_dir, "C:/state/auth");
	BOOST_CHECK_EQUAL(settings->jwt_access_ttl_minutes, 30u);
}

BOOST_AUTO_TEST_CASE(Test_OptionsAuth_InvalidModeRejected)
{
	Options::Auth::OptionsProcessor processor;
	auto vm = ParseArgs(processor, { "--auth-mode", "maybe" });

	BOOST_CHECK_THROW(std::ignore = processor.Process(vm), Exceptions::OptionParsingFailed);
}

BOOST_AUTO_TEST_CASE(Test_OptionsAuth_ZeroTtlRejected)
{
	Options::Auth::OptionsProcessor processor;
	auto vm = ParseArgs(processor, { "--jwt-access-ttl", "0" });

	BOOST_CHECK_THROW(std::ignore = processor.Process(vm), Exceptions::OptionParsingFailed);
}

BOOST_AUTO_TEST_SUITE_END()
