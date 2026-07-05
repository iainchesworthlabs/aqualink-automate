#pragma once

#include <chrono>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace AqualinkAutomate::Kernel { class HubLocator; }

namespace AqualinkAutomate::Jandy::Startup
{
	class JandyStartupEnvironment;
	class StartupCoordinator;

	// Drives the StartupCoordinator on the app's io_context: stands up the SerialAdapter
	// detector, observes the bus for a short detection window, then classifies the controller
	// and stands up the chosen emulation. A thin glue layer between the coordinator's pure
	// state machine and the cooperative run loop -- it owns the environment + coordinator and
	// re-arms a 1 s tick until the coordinator reaches Running.
	class JandyStartupService
	{
	public:
		JandyStartupService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, std::chrono::seconds chlorinator_setpoint_refresh_interval = std::chrono::seconds{ 300 });
		~JandyStartupService();

		void Start();   // Begin() the coordinator + arm the tick timer
		void Stop();    // cancel the timer

	private:
		void Tick();

	private:
		boost::asio::steady_timer m_Timer;
		std::unique_ptr<JandyStartupEnvironment> m_Environment;
		std::unique_ptr<StartupCoordinator> m_Coordinator;
		unsigned m_TicksElapsed{ 0 };
		bool m_Stopped{ false };

		static constexpr std::chrono::seconds TICK_INTERVAL{ 1 };
		static constexpr unsigned DETECTION_WINDOW_TICKS{ 5 };  // ~5s -- enough for a full probe cycle
	};

}
// namespace AqualinkAutomate::Jandy::Startup
