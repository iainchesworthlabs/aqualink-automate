#include <chrono>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/chlorinator_setpoint_refresh.h"
#include "jandy/devices/chlorinator_setpoint_stale_detector.h"

using AqualinkAutomate::Devices::ChlorinatorSetpointRefresh;
using AqualinkAutomate::Devices::ChlorinatorSetpointStaleDetector;
using Action = AqualinkAutomate::Devices::ChlorinatorSetpointRefresh::Action;

namespace
{
	using namespace std::chrono_literals;

	// A non-trivial base time so "interval since epoch" never accidentally reads as elapsed.
	const ChlorinatorSetpointRefresh::TimePoint BASE = ChlorinatorSetpointRefresh::TimePoint{} + 1000h;

	// Build a configured + startup-complete + timer-seeded refresh, ready for the steady-state
	// matrix tests (mirrors what the device does on entering NormalOperation).
	ChlorinatorSetpointRefresh MakeReady(std::chrono::seconds interval, ChlorinatorSetpointRefresh::TimePoint seeded_at)
	{
		ChlorinatorSetpointRefresh r;
		r.Configure(interval);
		r.MarkStartupComplete();
		r.NotifyScrapeFinished(seeded_at);   // seed the interval timer
		return r;
	}
}

BOOST_AUTO_TEST_SUITE(ChlorinatorSetpointRefresh_TestSuite)

// --- Configuration gating ----------------------------------------------------

BOOST_AUTO_TEST_CASE(Unconfigured_NeverScrapes)
{
	ChlorinatorSetpointRefresh r;          // default-constructed: disabled
	r.MarkStartupComplete();
	BOOST_CHECK(!r.IsEnabled());
	BOOST_CHECK(r.Evaluate(/*emul*/true, /*goal*/false, /*online*/false, BASE) == Action::None);
}

BOOST_AUTO_TEST_CASE(ZeroInterval_Disables)
{
	ChlorinatorSetpointRefresh r;
	r.Configure(0s);
	r.MarkStartupComplete();
	BOOST_CHECK(!r.IsEnabled());
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 100h) == Action::None);
}

// --- Hard gates --------------------------------------------------------------

BOOST_AUTO_TEST_CASE(NotEmulating_NeverScrapes)
{
	auto r = MakeReady(300s, BASE);
	BOOST_CHECK(r.Evaluate(/*emul*/false, false, false, BASE + 400s) == Action::None);
}

BOOST_AUTO_TEST_CASE(GoalInProgress_Defers)
{
	auto r = MakeReady(300s, BASE);
	BOOST_CHECK(r.Evaluate(true, /*goal*/true, false, BASE + 400s) == Action::None);
}

BOOST_AUTO_TEST_CASE(StartupNotComplete_NeverScrapes)
{
	ChlorinatorSetpointRefresh r;
	r.Configure(300s);
	r.NotifyScrapeFinished(BASE);   // seeded, but startup never marked complete
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 400s) == Action::None);
}

// --- Timing ------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(IntervalNotElapsed_NoScrape)
{
	auto r = MakeReady(300s, BASE);
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 100s) == Action::None);
}

BOOST_AUTO_TEST_CASE(IntervalElapsed_Scrapes)
{
	auto r = MakeReady(300s, BASE);
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 301s) == Action::Scrape);
}

BOOST_AUTO_TEST_CASE(ScrapeStarted_ResetsIntervalTimer)
{
	auto r = MakeReady(300s, BASE);
	BOOST_REQUIRE(r.Evaluate(true, false, false, BASE + 301s) == Action::Scrape);

	r.NotifyScrapeStarted(BASE + 301s);
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 400s) == Action::None);   // only 99s since start
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 602s) == Action::Scrape); // 301s since start
}

// --- Comms-recovery one-shot -------------------------------------------------

BOOST_AUTO_TEST_CASE(FirstOnlineSample_IsBaseline_NotRecovery)
{
	auto r = MakeReady(300s, BASE);
	// First observation is online but well within the interval: it only establishes the
	// baseline and must NOT be mistaken for an offline->online recovery.
	BOOST_CHECK(r.Evaluate(true, false, /*online*/true, BASE + 10s) == Action::None);
}

BOOST_AUTO_TEST_CASE(OfflineToOnlineEdge_ForcesScrapeBeforeInterval)
{
	auto r = MakeReady(300s, BASE);
	BOOST_REQUIRE(r.Evaluate(true, false, /*online*/false, BASE + 10s) == Action::None);  // baseline: offline
	BOOST_CHECK(r.Evaluate(true, false, /*online*/true, BASE + 20s) == Action::Scrape);   // recovery edge

	// Starting the scrape consumes the one-shot; no further scrape until the interval elapses.
	r.NotifyScrapeStarted(BASE + 20s);
	BOOST_CHECK(r.Evaluate(true, false, /*online*/true, BASE + 30s) == Action::None);
}

BOOST_AUTO_TEST_CASE(RecoveryEdge_StillDefersWhileBusy)
{
	auto r = MakeReady(300s, BASE);
	BOOST_REQUIRE(r.Evaluate(true, false, false, BASE + 10s) == Action::None);  // baseline offline
	// A recovery edge arrives but a user/SET goal is in flight: must defer (None), and the
	// pending recovery is retained for the next free tick.
	BOOST_CHECK(r.Evaluate(true, /*goal*/true, /*online*/true, BASE + 20s) == Action::None);
	BOOST_CHECK(r.RecoveryPending());
	BOOST_CHECK(r.Evaluate(true, /*goal*/false, /*online*/true, BASE + 30s) == Action::Scrape);
}

// --- Stale-setpoint request ---------------------------------------------------

BOOST_AUTO_TEST_CASE(StaleRequest_ForcesScrapeBeforeInterval)
{
	auto r = MakeReady(300s, BASE);
	BOOST_CHECK(r.Evaluate(true, false, true, BASE + 10s) == Action::None);   // nothing due yet

	r.LatchStaleRequest();
	BOOST_CHECK(r.StalePending());
	BOOST_CHECK(r.Evaluate(true, false, true, BASE + 20s) == Action::Scrape);

	// Starting the scrape consumes the one-shot; the next one is a full interval away.
	r.NotifyScrapeStarted(BASE + 20s);
	BOOST_CHECK(!r.StalePending());
	BOOST_CHECK(r.Evaluate(true, false, true, BASE + 30s) == Action::None);
}

BOOST_AUTO_TEST_CASE(StaleRequest_StillHonoursEveryGate)
{
	auto r = MakeReady(300s, BASE);
	r.LatchStaleRequest();

	// Not emulating, or a goal in flight: must defer, and keep the request for later.
	BOOST_CHECK(r.Evaluate(/*emul*/false, false, true, BASE + 10s) == Action::None);
	BOOST_CHECK(r.Evaluate(true, /*goal*/true, true, BASE + 11s) == Action::None);
	BOOST_CHECK(r.StalePending());
	BOOST_CHECK(r.Evaluate(true, false, true, BASE + 12s) == Action::Scrape);
}

BOOST_AUTO_TEST_CASE(StaleRequest_NotHonouredBeforeStartupOrWhenDisabled)
{
	ChlorinatorSetpointRefresh before_startup;
	before_startup.Configure(300s);
	before_startup.LatchStaleRequest();
	BOOST_CHECK(before_startup.Evaluate(true, false, true, BASE) == Action::None);   // startup crawl still owns the menu

	auto disabled = MakeReady(300s, BASE);
	disabled.Disable();
	disabled.LatchStaleRequest();
	BOOST_CHECK(disabled.Evaluate(true, false, true, BASE + 400s) == Action::None);
}

// --- Permanent disable -------------------------------------------------------

BOOST_AUTO_TEST_CASE(Disable_PermanentlyStops)
{
	auto r = MakeReady(300s, BASE);
	r.Disable();
	BOOST_CHECK(!r.IsEnabled());
	BOOST_CHECK(r.Evaluate(true, false, false, BASE + 400s) == Action::None);
	// Even a recovery edge must not revive it.
	BOOST_CHECK(r.Evaluate(true, false, true, BASE + 500s) == Action::None);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// ChlorinatorSetpointStaleDetector: when is the configured target worth re-reading?
//=============================================================================

namespace
{
	using Observation = ChlorinatorSetpointStaleDetector::Observation;

	Observation Obs(std::optional<std::uint8_t> wire, std::optional<std::uint8_t> configured,
		std::optional<std::uint8_t> pool = std::nullopt, std::optional<std::uint8_t> spa = std::nullopt, bool command_recent = false)
	{
		Observation o;
		o.WirePercent = wire;
		o.ConfiguredSetpoint = configured;
		o.PoolSetpoint = pool;
		o.SpaSetpoint = spa;
		o.CommandRecent = command_recent;
		return o;
	}

	// Feed `count` identical frames ~0.4 s apart, returning how many of them asked for a refresh.
	int Feed(ChlorinatorSetpointStaleDetector& d, const Observation& o, int count, ChlorinatorSetpointRefresh::TimePoint& now)
	{
		int requests = 0;
		for (int i = 0; i < count; ++i)
		{
			now += 400ms;
			if (d.Observe(o, now)) { ++requests; }
		}
		return requests;
	}
}

BOOST_AUTO_TEST_SUITE(ChlorinatorSetpointStaleDetector_TestSuite)

BOOST_AUTO_TEST_CASE(AgreeingFrames_NeverAsk)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	BOOST_CHECK_EQUAL(Feed(d, Obs(65, 65, 65, 80), 50, now), 0);
}

BOOST_AUTO_TEST_CASE(SustainedDisagreement_AsksExactlyOnce)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;

	// The panel was changed to 70% while the dashboard still believes 65%.
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 4, now), 0);   // below the confidence threshold
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 1, now), 1);   // the 5th frame asks
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 200, now), 0); // and never again for the same situation
}

BOOST_AUTO_TEST_CASE(SingleStrayFrame_IsIgnored)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 1, now), 0);
	BOOST_CHECK_EQUAL(Feed(d, Obs(65, 65, 65, 80), 10, now), 0);   // back in step: streak resets
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 4, now), 0);    // a fresh streak is needed again
}

BOOST_AUTO_TEST_CASE(OwnPendingCommand_IsNotStaleness)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	// We just queued 80% (configured already written through); the wire still says the old 65%
	// while the panel walks its menus. That is expected, not a stale target.
	BOOST_CHECK_EQUAL(Feed(d, Obs(65, 80, 80, 80, /*command_recent*/true), 60, now), 0);
}

BOOST_AUTO_TEST_CASE(SentinelsAndZero_AreIgnored)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	BOOST_CHECK_EQUAL(Feed(d, Obs(std::nullopt, 65, 65, 80), 30, now), 0);   // boost / service-mode frame
	BOOST_CHECK_EQUAL(Feed(d, Obs(0, 65, 65, 80), 30, now), 0);              // idle or "off": says nothing
}

BOOST_AUTO_TEST_CASE(UnknownConfiguredTarget_Asks)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	// Never scraped (for example the startup crawl could not run): nothing to compare against.
	BOOST_CHECK_EQUAL(Feed(d, Obs(60, std::nullopt), 5, now), 1);
}

BOOST_AUTO_TEST_CASE(PanelServingTheOtherBody_AsksOnceNotForever)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	// The panel is in spa mode (wire = 80) but the app believes pool (65): the scrape reads pool 65
	// / spa 80 and changes nothing, so asking again would be a menu walk every minute for nothing.
	BOOST_CHECK_EQUAL(Feed(d, Obs(80, 65, 65, 80), 5, now), 1);
	BOOST_CHECK_EQUAL(Feed(d, Obs(80, 65, 65, 80), 1000, now), 0);
}

BOOST_AUTO_TEST_CASE(WireStillMoving_DoesNotBuildConfidence)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;

	// The commanded value is changing under us (three different disagreeing values, three frames
	// each): none of them has persisted long enough to be worth a menu walk.
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 3, now), 0);
	BOOST_CHECK_EQUAL(Feed(d, Obs(75, 65, 65, 80), 3, now), 0);
	BOOST_CHECK_EQUAL(Feed(d, Obs(80, 65, 65, 80), 3, now), 0);

	// It settles: the fifth consecutive frame of one value asks.
	BOOST_CHECK_EQUAL(Feed(d, Obs(80, 65, 65, 80), 2, now), 1);
}

BOOST_AUTO_TEST_CASE(NewSituationAfterCooldown_AsksAgain)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 5, now), 1);

	// A different disagreement inside the cooldown is held back...
	BOOST_CHECK_EQUAL(Feed(d, Obs(75, 65, 65, 80), 10, now), 0);

	// ...and allowed once the cooldown has passed.
	now += 61s;
	BOOST_CHECK_EQUAL(Feed(d, Obs(75, 65, 65, 80), 1, now), 1);
}

BOOST_AUTO_TEST_CASE(BackInStep_ReArmsForALaterChange)
{
	ChlorinatorSetpointStaleDetector d;
	auto now = BASE;
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 5, now), 1);

	// The scrape lands and the target now agrees; later the same wire value is a NEW event.
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 70, 70, 80), 10, now), 0);
	now += 61s;
	BOOST_CHECK_EQUAL(Feed(d, Obs(70, 65, 65, 80), 5, now), 1);
}

BOOST_AUTO_TEST_SUITE_END()
