#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/program_options.hpp>

#include "exceptions/exception_optionshelporversion.h"
#include "options/options.h"
#include "options/options_app_options.h"

//
// Coverage for the Application options area's one-shot CLI actions. The service
// install/uninstall lifecycle itself drives the Windows Service Control Manager and is a
// manual, elevated integration check (as --register-log-source is); what is unit-testable
// is that the options are declared and that the pipeline's early-exit wiring behaves.
//

using namespace AqualinkAutomate;

namespace
{
	/// Minimal pipeline through CheckHelpAndVersion (the stage that runs
	/// HandleServiceInstallation). Mirrors main's ordering; only the App area is needed.
	auto RunHelpCheck(const std::vector<const char*>& args)
	{
		return Options::Initialise()
			| Options::Add(Options::App::OptionsProcessor{})
			| Options::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data()))
			| Options::CheckHelpAndVersion()
			| Options::Finalise();
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_OptionsApp)

BOOST_AUTO_TEST_CASE(AppOptions_DeclareServiceActions, *boost::unit_test::label("unit"))
{
	// The install/uninstall flags must be registered so the command line parses them
	// instead of rejecting them as unknown options.
	const Options::App::OptionsProcessor processor;
	const auto description = processor.Options();

	bool has_install = false;
	bool has_uninstall = false;
	for (const auto& option : description.options())
	{
		if ("install-service" == option->long_name()) { has_install = true; }
		if ("uninstall-service" == option->long_name()) { has_uninstall = true; }
	}

	BOOST_TEST(has_install);
	BOOST_TEST(has_uninstall);
}

BOOST_AUTO_TEST_CASE(AppOptions_InstallAndUninstallTogetherIsRejected, *boost::unit_test::label("unit"))
{
	// Supplying both actions at once short-circuits with the shared help/version exit
	// BEFORE any Service Control Manager call, so this is safe to exercise on every
	// platform (no service is created).
	BOOST_CHECK_THROW(
		(void)RunHelpCheck({ "program", "--install-service", "--uninstall-service" }),
		Exceptions::OptionsHelpOrVersion);
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(AppOptions_ServiceActionsAreWindowsOnlyElsewhere, *boost::unit_test::label("unit"))
{
	// On non-Windows the action prints "only supported on Windows" and exits via the
	// same one-shot mechanism as --help/--version. (On Windows this path would call the
	// SCM, so it is a manual elevated check there, not a unit test.)
	BOOST_CHECK_THROW(
		(void)RunHelpCheck({ "program", "--install-service" }),
		Exceptions::OptionsHelpOrVersion);
	BOOST_CHECK_THROW(
		(void)RunHelpCheck({ "program", "--uninstall-service" }),
		Exceptions::OptionsHelpOrVersion);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
