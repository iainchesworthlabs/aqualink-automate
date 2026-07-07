#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <boost/signals2.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "mqtt/ha_discovery.h"
#include "mqtt/mqtt_hub.h"
#include "options/options_mqtt_options.h"

namespace AqualinkAutomate::Mqtt
{

	/// High-level MQTT integration class that orchestrates MQTT functionality.
	/// Provides a simple interface for the main application to enable MQTT
	/// data publishing and command handling.
	class MqttIntegration
	{
	public:
		explicit MqttIntegration(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings);
		~MqttIntegration() = default;

		// Non-copyable, non-movable
		MqttIntegration(const MqttIntegration&) = delete;
		MqttIntegration& operator=(const MqttIntegration&) = delete;
		MqttIntegration(MqttIntegration&&) = delete;
		MqttIntegration& operator=(MqttIntegration&&) = delete;

		//---------------------------------------------------------------------
		// LIFECYCLE
		//---------------------------------------------------------------------

		/// Start the MQTT integration. Only starts if enabled in settings.
		void Start();

		/// Stop the MQTT integration.
		void Stop();

		/// Poll the MQTT integration (forwards to hub).
		void Poll();

		/// Test seam: override the monotonic clock backing the HA-discovery seed-grace
		/// deadline so that timed Poll() branch can be driven deterministically without
		/// a real wait. Defaults to std::chrono::steady_clock::now.
		using SteadyClockFn = std::function<std::chrono::steady_clock::time_point()>;
		void SetSteadyClock(SteadyClockFn clock) { m_SteadyNow = std::move(clock); }

		/// Check if MQTT is enabled in settings.
		bool IsEnabled() const;

		/// Check if MQTT is currently running and connected.
		bool IsRunning() const;

		//---------------------------------------------------------------------
		// HUB CONNECTIONS
		//---------------------------------------------------------------------

		/// Connect to all hubs using the hub locator.
		void ConnectHubs(Kernel::HubLocator& hub_locator);

		/// Connect to individual hubs. The preferences hub is optional (may be
		/// nullptr): without it the HA setpoint number entities stay Celsius.
		void ConnectHubs(
			const std::shared_ptr<Kernel::DataHub>& data_hub,
			const std::shared_ptr<Kernel::EquipmentHub>& equipment_hub,
			const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub,
			const std::shared_ptr<Kernel::PreferencesHub>& preferences_hub = nullptr);

		//---------------------------------------------------------------------
		// ACCESS
		//---------------------------------------------------------------------

		/// Get the underlying MQTT hub for advanced use cases.
		std::shared_ptr<MqttHub> GetMqttHub() const;

	private:
		//---------------------------------------------------------------------
		// DEFAULT COMMAND HANDLERS
		//---------------------------------------------------------------------

		void RegisterDefaultCommands();
		void RegisterDeviceCommand();
		void RegisterSetpointCommand();
		void RegisterDynamicDeviceCommands();

	private:
		const Options::Mqtt::MqttSettings m_Settings;
		std::shared_ptr<MqttHub> m_Hub;

		// Home Assistant discovery
		std::shared_ptr<HomeAssistantDiscovery> m_HaDiscovery;
		boost::signals2::scoped_connection m_HaConnectedConnection;
		boost::signals2::scoped_connection m_HaDevicesConnection;

		// HA discovery-config seed: on connect we read back the broker's existing retained
		// discovery config BEFORE publishing a new one, so entities for devices that no longer
		// exist can be tombstoned (removed from HA). The first discovery publish is deferred until
		// that config arrives or the grace window elapses (a fresh broker has none).
		boost::signals2::scoped_connection m_HaConfigSeedConnection;
		bool m_HaSeedPending{ false };
		std::chrono::steady_clock::time_point m_HaSeedDeadline;
		std::string m_HaConfigTopic;
		static constexpr std::chrono::seconds HA_SEED_GRACE{ 3 };

		// Monotonic clock for the HA seed-grace deadline; overridable in tests via
		// SetSteadyClock(). Production uses the real steady_clock.
		SteadyClockFn m_SteadyNow{ [] { return std::chrono::steady_clock::now(); } };

		// Weak references to connected hubs
		std::weak_ptr<Kernel::DataHub> m_DataHub;
		std::weak_ptr<Kernel::EquipmentHub> m_EquipmentHub;
		std::weak_ptr<Kernel::StatisticsHub> m_StatisticsHub;
		std::weak_ptr<Kernel::PreferencesHub> m_PreferencesHub;
		std::weak_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher;

		// Republishes the HA discovery configs when Temperature_DisplayUnits
		// changes (the setpoint number entities' unit/range follow it).
		boost::signals2::scoped_connection m_PrefsUnitsConnection;
	};

	/// Factory function to create MqttIntegration from settings.
	/// Returns nullptr if MQTT is not enabled.
	std::shared_ptr<MqttIntegration> CreateMqttIntegration(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings);

}
// namespace AqualinkAutomate::Mqtt
