#pragma once

#include <chrono>

namespace AqualinkAutomate::Devices
{

	// Decision logic for proactively re-acquiring the configured chlorinator (AquaPure)
	// setpoint by navigating the iAQ/OneTouch menu to the "Set AquaPure" page and scraping
	// it.  The AquaRite/AquaPure RS-485 protocol has no "get setpoint" query, and the live
	// AQUARITE_Percent (0x11) byte only carries the instantaneous output (0 while idle), so
	// the configured Pool/Spa % can only be read by visiting that menu page - which is only
	// possible while actively emulating an iAQ/OneTouch device.
	//
	// This struct holds ALL the "when should we scrape" policy so it can be unit-tested in
	// isolation (no device, no bus): the caller passes the current gating booleans and a
	// clock value to Evaluate(), and drives the result.  The mechanism that actually walks
	// the menu lives in the device (OneTouchDevice::SetpointRefresh_ProcessStep).
	//
	// Triggers: a periodic interval, an offline->online (comms-recovery) edge as a one-shot,
	// and (implicitly) the very first eligible tick after startup once the timer is seeded.
	class ChlorinatorSetpointRefresh
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;

		enum class Action
		{
			None,    // do nothing this tick
			Scrape   // start a read-only menu visit to re-scrape the setpoint
		};

		// Configure the periodic interval.  interval == 0 disables periodic refresh entirely
		// (the comms-recovery and startup triggers are also suppressed when disabled, since a
		// 0 interval is the operator's way of saying "never proactively navigate").
		constexpr void Configure(std::chrono::seconds interval)
		{
			m_Interval = interval;
			m_Enabled = (interval > std::chrono::seconds::zero());
		}

		// Permanently disable refreshing (e.g. the startup crawl proved there is no chlorinator
		// / Set AquaPure page on this panel, so periodic navigation would only ever fail).
		constexpr void Disable()
		{
			m_Enabled = false;
		}

		// The startup menu crawl has finished (it already visits Set AquaPure, so the setpoint
		// is freshly known); periodic refresh becomes eligible from here.
		constexpr void MarkStartupComplete()
		{
			m_StartupComplete = true;
		}

		// A refresh visit has just been kicked off: consume the one-shot recovery flag and seed
		// the interval timer from now so the next periodic scrape is a full interval away.
		constexpr void NotifyScrapeStarted(TimePoint now)
		{
			m_RecoveryPending = false;
			m_LastScrape = now;
		}

		// A refresh visit finished (success OR failure): reset the interval timer so the next
		// attempt is a full interval away (a failed scrape therefore backs off, never spams).
		constexpr void NotifyScrapeFinished(TimePoint now)
		{
			m_LastScrape = now;
		}

		// Decide whether to start a refresh this tick.  Latches the offline->online edge into a
		// pending one-shot regardless of the other gates, then applies the hard gates (enabled,
		// configured, actively emulating, menu free of any user/SET goal, startup finished) and
		// finally the timing (interval elapsed, or a pending recovery).
		constexpr Action Evaluate(bool emulation_active, bool goal_in_progress, bool chlorinator_online, TimePoint now)
		{
			// Latch a genuine offline->online transition as a one-shot recovery scrape. The very
			// first observation only establishes the baseline (the chlorinator may already be
			// online at startup, which the startup crawl has already scraped), so it must NOT be
			// mistaken for a recovery.
			if (m_OnlineInitialised && chlorinator_online && !m_PrevOnline)
			{
				m_RecoveryPending = true;
			}
			m_PrevOnline = chlorinator_online;
			m_OnlineInitialised = true;

			if (!m_Enabled || m_Interval <= std::chrono::seconds::zero() || !emulation_active || goal_in_progress || !m_StartupComplete)
			{
				return Action::None;
			}

			if (const bool interval_elapsed = (now - m_LastScrape) >= m_Interval; m_RecoveryPending || interval_elapsed)
			{
				return Action::Scrape;
			}

			return Action::None;
		}

		// Accessors (primarily for tests / diagnostics).
		constexpr bool IsEnabled() const { return m_Enabled; }
		constexpr std::chrono::seconds Interval() const { return m_Interval; }
		constexpr bool StartupComplete() const { return m_StartupComplete; }
		constexpr bool RecoveryPending() const { return m_RecoveryPending; }

	private:
		bool m_Enabled{ false };
		std::chrono::seconds m_Interval{ 0 };
		bool m_StartupComplete{ false };
		bool m_PrevOnline{ false };
		bool m_OnlineInitialised{ false };   // false until the first online sample establishes the baseline
		bool m_RecoveryPending{ false };
		TimePoint m_LastScrape{};   // default = epoch -> first eligible tick scrapes unless seeded
	};

}
// namespace AqualinkAutomate::Devices
