#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>

#include "logging/sinks/log_environment.h"

//
// Coverage for the logging environment detection and the `auto` sink policy
// (docs/logging-sinks-redesign.md §6). ResolveAutoSinks() and JournalStreamMatches()
// are pure, so the whole §6.2 truth table and every JOURNAL_STREAM edge case are
// exercised without touching a real terminal, journal, or syscall. DetectLogEnvironment()
// is driven through injected probes.
//

using namespace AqualinkAutomate::Logging::Sinks;

BOOST_AUTO_TEST_SUITE(TestSuite_LogEnvironment)

//-----------------------------------------------------------------------------
// JOURNAL_STREAM parsing
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(JournalStream_MatchesWhenDeviceAndInodeEqual, *boost::unit_test::label("unit"))
{
	BOOST_TEST(JournalStreamMatches(std::string{ "42:1337" }, DevIno{ 42, 1337 }));
}

BOOST_AUTO_TEST_CASE(JournalStream_MismatchOnDeviceOrInode, *boost::unit_test::label("unit"))
{
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:1337" }, DevIno{ 43, 1337 }));
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:1337" }, DevIno{ 42, 1338 }));
}

BOOST_AUTO_TEST_CASE(JournalStream_FailsClosedOnMissingInputs, *boost::unit_test::label("unit"))
{
	BOOST_TEST(!JournalStreamMatches(std::nullopt, DevIno{ 42, 1337 }));
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:1337" }, std::nullopt));
	BOOST_TEST(!JournalStreamMatches(std::nullopt, std::nullopt));
}

BOOST_AUTO_TEST_CASE(JournalStream_FailsClosedOnMalformedValue, *boost::unit_test::label("unit"))
{
	const DevIno stat{ 42, 1337 };

	BOOST_TEST(!JournalStreamMatches(std::string{ "" }, stat));         // empty
	BOOST_TEST(!JournalStreamMatches(std::string{ "421337" }, stat));   // no separator
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:" }, stat));      // missing inode
	BOOST_TEST(!JournalStreamMatches(std::string{ ":1337" }, stat));    // missing device
	BOOST_TEST(!JournalStreamMatches(std::string{ "4x:1337" }, stat));  // non-numeric device
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:13x7" }, stat));  // non-numeric inode
	BOOST_TEST(!JournalStreamMatches(std::string{ "42:1337:9" }, stat));// trailing junk (inode part not fully numeric)
}

//-----------------------------------------------------------------------------
// auto policy resolution (§6.2)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Auto_InteractiveTty_ConsoleOnly, *boost::unit_test::label("unit"))
{
	LogEnvironment env;
	env.StderrIsTty = true;

	const auto sel = ResolveAutoSinks(env, /*have_log_file*/ false, /*journald_available*/ false);

	BOOST_TEST(sel.Console);
	BOOST_TEST(!sel.ConsoleJournaldPrefixes);
	BOOST_TEST(!sel.Native);
	BOOST_TEST(!sel.File);
}

BOOST_AUTO_TEST_CASE(Auto_Journal_ConsoleWithPrefixesNoNative, *boost::unit_test::label("unit"))
{
	LogEnvironment env;
	env.StderrIsJournal = true;

	const auto sel = ResolveAutoSinks(env, false, /*journald_available*/ false);

	BOOST_TEST(sel.Console);
	BOOST_TEST(sel.ConsoleJournaldPrefixes);
	BOOST_TEST(!sel.Native);   // a syslog sink here would double-log via the journal
}

BOOST_AUTO_TEST_CASE(Auto_Journal_UsesJournaldSinkWhenAvailable, *boost::unit_test::label("unit"))
{
	LogEnvironment env;
	env.StderrIsJournal = true;

	const auto sel = ResolveAutoSinks(env, false, /*journald_available*/ true);

	// The structured journald sink replaces the console (which would double-log into
	// the journal): no console, no "<N>" prefixes, no general native sink.
	BOOST_TEST(sel.Journald);
	BOOST_TEST(!sel.Console);
	BOOST_TEST(!sel.ConsoleJournaldPrefixes);
	BOOST_TEST(!sel.Native);
}

BOOST_AUTO_TEST_CASE(Auto_WindowsService_ConsolePlusNative, *boost::unit_test::label("unit"))
{
	LogEnvironment env;
	env.WindowsServiceContext = true;

	const auto sel = ResolveAutoSinks(env, false, /*journald_available*/ false);

	BOOST_TEST(sel.Console);
	BOOST_TEST(sel.Native);
	BOOST_TEST(!sel.ConsoleJournaldPrefixes);
}

BOOST_AUTO_TEST_CASE(Auto_PipeOrContainer_ConsoleOnly, *boost::unit_test::label("unit"))
{
	// Every axis false: redirected/piped/containerised stderr. Console owns nothing
	// but delivery; the log driver / redirect target owns storage.
	LogEnvironment env;

	const auto sel = ResolveAutoSinks(env, false, /*journald_available*/ false);

	BOOST_TEST(sel.Console);
	BOOST_TEST(!sel.ConsoleJournaldPrefixes);
	BOOST_TEST(!sel.Native);
	BOOST_TEST(!sel.File);
}

BOOST_AUTO_TEST_CASE(Auto_LogFilePresent_AddsFileSinkInEveryArm, *boost::unit_test::label("unit"))
{
	LogEnvironment tty; tty.StderrIsTty = true;
	LogEnvironment journal; journal.StderrIsJournal = true;
	LogEnvironment service; service.WindowsServiceContext = true;
	LogEnvironment plain;

	BOOST_TEST(ResolveAutoSinks(tty, true, /*journald_available*/ false).File);
	BOOST_TEST(ResolveAutoSinks(journal, true, /*journald_available*/ false).File);
	BOOST_TEST(ResolveAutoSinks(service, true, /*journald_available*/ false).File);
	BOOST_TEST(ResolveAutoSinks(plain, true, /*journald_available*/ false).File);
}

//-----------------------------------------------------------------------------
// DetectLogEnvironment with injected probes
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Detect_JournalDetectedWhenStreamMatchesStat, *boost::unit_test::label("unit"))
{
	EnvironmentProbes probes;
	probes.StderrIsTty = [] { return false; };
	probes.GetEnvVar = [](const char* name) -> std::optional<std::string>
	{
		return std::string{ name } == "JOURNAL_STREAM" ? std::optional<std::string>{ "7:99" } : std::nullopt;
	};
	probes.StatStderr = [] { return std::optional<DevIno>{ DevIno{ 7, 99 } }; };
	probes.WindowsServiceContext = [] { return false; };

	const auto env = DetectLogEnvironment(probes);

	BOOST_TEST(env.StderrIsJournal);
	BOOST_TEST(!env.StderrIsTty);
	BOOST_TEST(!env.WindowsServiceContext);
}

BOOST_AUTO_TEST_CASE(Detect_NoJournalWhenStreamMismatches, *boost::unit_test::label("unit"))
{
	EnvironmentProbes probes;
	probes.StderrIsTty = [] { return true; };
	probes.GetEnvVar = [](const char*) -> std::optional<std::string> { return std::string{ "7:99" }; };
	probes.StatStderr = [] { return std::optional<DevIno>{ DevIno{ 7, 100 } }; };  // inode differs
	probes.WindowsServiceContext = [] { return false; };

	const auto env = DetectLogEnvironment(probes);

	BOOST_TEST(!env.StderrIsJournal);
	BOOST_TEST(env.StderrIsTty);
}

BOOST_AUTO_TEST_CASE(Detect_WindowsServiceContextProbeDrivesEventLogSink, *boost::unit_test::label("unit"))
{
	// The seam the Windows service host relies on: RunApplication overrides only the
	// WindowsServiceContext probe (from AppHostHooks::RunningAsManagedService) and feeds
	// the result to DetectLogEnvironment(probes). A true probe must set the flag, which
	// the `auto` policy then resolves to the Console + Event Log (Native) sink pair.
	EnvironmentProbes probes;
	probes.StderrIsTty = [] { return false; };
	probes.GetEnvVar = [](const char*) -> std::optional<std::string> { return std::nullopt; };
	probes.StatStderr = [] { return std::optional<DevIno>{}; };
	probes.WindowsServiceContext = [] { return true; };

	const auto env = DetectLogEnvironment(probes);
	BOOST_TEST(env.WindowsServiceContext);

	const auto sel = ResolveAutoSinks(env, /*have_log_file*/ false, /*journald_available*/ false);
	BOOST_TEST(sel.Console);
	BOOST_TEST(sel.Native);
}

BOOST_AUTO_TEST_SUITE_END()
