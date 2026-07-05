#include "startup/jandy_startup_service.h"

#include <format>

#include "logging/logging.h"
#include "startup/jandy_startup_coordinator.h"
#include "startup/jandy_startup_environment.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Jandy::Startup
{

	JandyStartupService::JandyStartupService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, std::chrono::seconds chlorinator_setpoint_refresh_interval)
		: m_IoContext(io_context)
		, m_Timer(io_context)
		, m_Environment(std::make_unique<JandyStartupEnvironment>(hub_locator, chlorinator_setpoint_refresh_interval))
		, m_Coordinator(std::make_unique<StartupCoordinator>(*m_Environment))
	{
	}

	JandyStartupService::~JandyStartupService() = default;

	void JandyStartupService::Start()
	{
		LogInfo(Channel::Main, "Jandy auto-startup: standing up a SerialAdapter and classifying the controller from the bus");
		m_Coordinator->Begin();
		Tick();
	}

	void JandyStartupService::Stop()
	{
		m_Stopped = true;
		m_Timer.cancel();
	}

	void JandyStartupService::Tick()
	{
		if (m_Stopped)
		{
			return;
		}

		++m_TicksElapsed;

		if (const bool window_elapsed = (m_TicksElapsed >= DETECTION_WINDOW_TICKS); m_Coordinator->Advance(window_elapsed) == StartupCoordinator::Phase::Running)
		{
			LogInfo(Channel::Main, std::format("Jandy auto-startup complete: {}", m_Coordinator->Plan().rationale));
			return;  // reached steady state -- stop ticking
		}

		m_Timer.expires_after(TICK_INTERVAL);
		m_Timer.async_wait([this](const boost::system::error_code& ec)
		{
			if (!ec)
			{
				Tick();
			}
		});
	}

}
// namespace AqualinkAutomate::Jandy::Startup
