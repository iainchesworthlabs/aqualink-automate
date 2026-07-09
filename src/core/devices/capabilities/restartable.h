#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace AqualinkAutomate::Devices::Capabilities
{

	// Protocol-agnostic watchdog capability.  A device that mixes in Restartable
	// is "alive" only while it keeps Kick()-ing the watchdog within the timeout
	// window; once the deadline elapses without a Kick(), WatchdogTimeoutOccurred()
	// fires.  Used by both Jandy and Pentair device handlers (and any future
	// protocol), so it lives in the shared core library rather than in a
	// protocol-specific one.
	class Restartable
	{
		static constexpr std::chrono::seconds DEFAULT_WATCHDOG_TIMEOUT{ std::chrono::seconds(30) };

	protected:
		explicit Restartable(std::chrono::seconds timeout_in_seconds = DEFAULT_WATCHDOG_TIMEOUT, bool delayed_start = false);
		virtual ~Restartable();

	public:
		// Evaluates the watchdog deadline for every live Restartable instance and fires
		// WatchdogTimeoutOccurred() on any that have expired. Call once per frame.
		// Reads the wall clock exactly once and shares it across every instance, rather
		// than each instance re-reading steady_clock::now() during its own deadline check.
		static void PollAll();

	protected:
		void Start();
		void Kick();
		void Stop();

		std::chrono::seconds GetTimeout() const;
		bool IsRunning() const;

		// Fired when the watchdog deadline elapses without an intervening Kick().
		virtual void WatchdogTimeoutOccurred() = 0;

		// Human-readable identity for this watchdog (e.g. "<class> @ id 0x<id>"), used in the
		// base timeout warning so the log line is actionable on a bus with many devices.
		// Defaults to a generic label; device handlers override to report class + bus id.
		virtual std::string WatchdogName() const;

		// Clock used for deadline evaluation; overridable so tests can inject time.
		virtual std::chrono::steady_clock::time_point Now() const;

		// Evaluates this instance's deadline against the supplied time point; fires
		// WatchdogTimeoutOccurred() on expiry. Taking 'now' as a parameter lets PollAll()
		// read the clock once per sweep and share it across every instance.
		void CheckWatchdog(std::chrono::steady_clock::time_point now);

	private:
		const std::chrono::seconds m_TimeoutDuration;
		bool m_IsRunning{ false };
		std::chrono::steady_clock::time_point m_LastKick;

		static std::vector<Restartable*> s_Instances;
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
