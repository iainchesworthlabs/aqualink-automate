#include <algorithm>
#include <format>
#include <string>

#include "devices/capabilities/restartable.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices::Capabilities
{
	const std::chrono::seconds Restartable::DEFAULT_WATCHDOG_TIMEOUT{ std::chrono::seconds(30) };

	std::vector<Restartable*> Restartable::s_Instances;

	Restartable::Restartable(std::chrono::seconds timeout_in_seconds, bool delayed_start) :
		m_TimeoutDuration(timeout_in_seconds)
	{
		s_Instances.push_back(this);

		if (!delayed_start)
		{
			m_IsRunning = true;

			// Construction-order: a subclass override of the virtual Now() seam is not
			// yet active during this base constructor, so read the real clock directly
			// instead of dispatching through Start() -> Kick() -> Now().  This is
			// behaviour-identical to the old eager Start() (production devices never
			// override Now(); clock-injecting test doubles use delayed_start=true and
			// call Start() after construction) while avoiding a virtual call from a
			// constructor (cpp:S1699).
			m_LastKick = std::chrono::steady_clock::now();
		}
	}

	Restartable::~Restartable()
	{
		Stop();
		std::erase(s_Instances, this);
	}

	void Restartable::PollAll()
	{
		// Read the clock once per sweep and share it across every instance instead of having
		// each instance re-read steady_clock::now() during its own deadline evaluation.
		const std::chrono::steady_clock::time_point now{ std::chrono::steady_clock::now() };

		// A copy is not required: CheckWatchdog() never adds or removes instances.
		for (auto* instance : s_Instances)
		{
			instance->CheckWatchdog(now);
		}
	}

	void Restartable::Start()
	{
		if (m_IsRunning)
		{
			LogTrace(Channel::Devices, "Attempted to start watchdog that is already running; ignoring start request");
		}
		else
		{
			m_IsRunning = true;

			// Note the order here....if m_Running is _NOT_ true, Kick() will not action the request...

			Kick();
		}
	}

	void Restartable::Kick()
	{
		if (m_IsRunning)
		{
			m_LastKick = Now();
		}
		else
		{
			LogTrace(Channel::Devices, "Attempted to kick watchdog that is stopped; ignoring kick request");
		}
	}

	void Restartable::Stop()
	{
		m_IsRunning = false;
	}

	void Restartable::CheckWatchdog(std::chrono::steady_clock::time_point now)
	{
		if (m_IsRunning && ((now - m_LastKick) > m_TimeoutDuration))
		{
			// No message has arrived within the timeout duration; mark this device as not operating.
			LogWarning(Channel::Devices, [this]()
				{
					return std::format("Device timeout: watchdog for {} expired (no activity for {} seconds)", WatchdogName(), m_TimeoutDuration.count());
				});

			m_IsRunning = false;
			WatchdogTimeoutOccurred();
		}
	}

	std::chrono::seconds Restartable::GetTimeout() const
	{
		return m_TimeoutDuration;
	}

	bool Restartable::IsRunning() const
	{
		return m_IsRunning;
	}

	std::string Restartable::WatchdogName() const
	{
		// Generic fallback; device handlers override this to report their class and bus id.
		return "unidentified device";
	}

	std::chrono::steady_clock::time_point Restartable::Now() const
	{
		return std::chrono::steady_clock::now();
	}

}
// namespace AqualinkAutomate::Devices::Capabilities
