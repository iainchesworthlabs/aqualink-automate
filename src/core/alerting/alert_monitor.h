#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

#include "alerting/alert_condition.h"
#include "options/options_alerting_options.h"

namespace AqualinkAutomate::Kernel
{
	class DataHub;
	class EquipmentHub;
	class StatisticsHub;
	class PreferencesHub;
	class HubLocator;
}
// namespace AqualinkAutomate::Kernel

namespace AqualinkAutomate::Alerting
{

	//=========================================================================
	// AlertMonitor — a plain io_context service (not a hub) that watches for
	// fault conditions and emits raised/cleared transitions to a set of sinks.
	//
	// Conditions are LATCHED: a transition fires only on an edge (cleared->raised
	// or raised->cleared), never on repeats.  Evaluation is re-derived from live
	// hub state, so the monitor is stateless across restarts — conditions simply
	// re-evaluate from whatever the hubs currently report.
	//
	// Sinks (MQTT problem entities, optional webhook, the WebSocket/UI channel)
	// are registered as callbacks so the engine stays decoupled and unit-testable
	// in isolation.  The clock is injectable for deterministic tests.
	//=========================================================================
	class AlertMonitor
	{
	public:
		using Sink = std::function<void(const AlertTransition&)>;
		using ClockFn = std::function<std::int64_t()>;   // returns unix seconds (UTC)

		AlertMonitor(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Alerting::AlertingSettings& settings);
		~AlertMonitor();

		AlertMonitor(const AlertMonitor&) = delete;
		AlertMonitor& operator=(const AlertMonitor&) = delete;

		// Register a sink invoked once per condition transition.
		void AddSink(Sink sink);

		// Subscribe to hub signals and start the comms-timeout timer.
		void Start();

		// Disconnect signals and cancel the timer.
		void Stop();

		// Re-evaluate every condition against current hub state.  Called on hub
		// events and on each comms-timer tick; also a deterministic test seam.
		void EvaluateAll();

		// Individual evaluators (exposed for targeted unit tests).
		void EvaluateSaltLow();
		void EvaluateChlorinatorWarning();
		void EvaluateChlorinatorFault();
		void EvaluateServiceMode();
		void EvaluateSerialCommsLoss();
		void EvaluateTemperatureStale();

		// Inject a deterministic clock (unix seconds).  Must be called before
		// Start() in tests that assert timing-dependent behaviour.
		void SetClock(ClockFn clock) { m_Clock = std::move(clock); }

		// Current latched state of a condition (false if unknown).
		bool IsRaised(std::string_view condition) const;

		// Consolidated state document: { "<condition>": "true"|"false", ... } for
		// every catalogue condition.  This is what the HA binary_sensors read.
		nlohmann::json BuildStateJson() const;

	private:
		void SetCondition(std::string_view key, bool raised, std::string detail, nlohmann::json params = nlohmann::json::object());
		void ScheduleCommsTimer();

	private:
		Options::Alerting::AlertingSettings m_Settings;

		std::shared_ptr<Kernel::DataHub> m_DataHub;
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
		std::shared_ptr<Kernel::StatisticsHub> m_StatisticsHub;
		std::shared_ptr<Kernel::PreferencesHub> m_PreferencesHub;

		// condition key -> raised?  Transparent comparator allows string_view lookup.
		std::map<std::string, bool, std::less<>> m_Latched;

		std::vector<Sink> m_Sinks;
		ClockFn m_Clock;

		boost::asio::steady_timer m_CommsTimer;
		bool m_Running{ false };

		// serial_comms_loss tracking: the last observed total message count and the
		// wall-clock second at which it last changed.
		std::uint64_t m_LastMessageCount{ 0 };
		std::int64_t m_LastMessageChangeTs{ 0 };
		bool m_HaveCommsBaseline{ false };

		boost::signals2::scoped_connection m_DataHubConn;
		boost::signals2::scoped_connection m_EquipmentConn;
	};

}
// namespace AqualinkAutomate::Alerting
