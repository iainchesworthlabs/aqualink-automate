#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <boost/asio/io_context.hpp>
#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

#include "interfaces/ihub.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/statistics_hub.h"
#include "mqtt/mqtt_client.h"
#include "options/options_mqtt_options.h"
#include "utility/json_serialization_helpers.h"

namespace AqualinkAutomate::Test { class MqttHubReconcileTest; }

namespace AqualinkAutomate::Mqtt
{

	/// MQTT Hub integrates pool controller data from the kernel hubs and publishes
	/// it to MQTT topics. It also handles incoming command messages.
	///
	/// Topic structure:
	/// - {prefix}/status/availability     - Online/offline status (LWT, retained)
	/// - {prefix}/system/status           - System online + uptime (retained)
	/// - {prefix}/system/version          - Application version info (retained, static)
	/// - {prefix}/system/equipment        - Equipment model/firmware (retained, static)
	/// - {prefix}/pool/temperatures       - Temperature readings and setpoints (retained)
	/// - {prefix}/pool/chemistry          - ORP, pH, salt (retained)
	/// - {prefix}/pool/circulation        - Circulation mode, spa mode (retained)
	/// - {prefix}/pool/configuration      - Pool type, system board, date/time (retained)
	/// - {prefix}/device/{slug}           - Per-device state (retained)
	/// - {prefix}/statistics/messages     - Message counts (not retained)
	/// - {prefix}/statistics/bandwidth    - Bandwidth metrics (not retained)
	/// - {prefix}/statistics/latency      - Latency percentiles (not retained)
	/// - {prefix}/statistics/serial       - Serial error metrics (not retained)
	/// - {prefix}/event/{type}            - Real-time events
	/// - {prefix}/command/{action}        - Command topics for control
	class MqttHub : public std::enable_shared_from_this<MqttHub>
	{
	public:
		using CommandHandler = std::function<void(const std::string& topic, const nlohmann::json& payload)>;

	public:
		explicit MqttHub(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings);
		~MqttHub();

		// Non-copyable, non-movable
		MqttHub(const MqttHub&) = delete;
		MqttHub& operator=(const MqttHub&) = delete;
		MqttHub(MqttHub&&) = delete;
		MqttHub& operator=(MqttHub&&) = delete;

	public:
		//---------------------------------------------------------------------
		// LIFECYCLE
		//---------------------------------------------------------------------

		/// Start the MQTT hub and begin publishing data.
		void Start();

		/// Stop the MQTT hub.
		void Stop();

		/// Poll the MQTT hub (advances client state machine and periodic publishing).
		void Poll();

		/// Check if the hub is running and connected.
		bool IsRunning() const;

		/// Test seam: override the monotonic clock backing Poll()'s debounce,
		/// startup-reconcile and periodic-publish deadlines, so those timed branches
		/// can be driven deterministically without real waits. Defaults to
		/// std::chrono::steady_clock::now.
		using SteadyClockFn = std::function<std::chrono::steady_clock::time_point()>;
		void SetSteadyClock(SteadyClockFn clock) { m_SteadyNow = std::move(clock); }

		//---------------------------------------------------------------------
		// HUB INTEGRATION
		//---------------------------------------------------------------------

		/// Connect to the data hub to receive pool configuration data.
		void ConnectDataHub(const std::shared_ptr<Kernel::DataHub>& data_hub);

		/// Connect to the equipment hub to receive equipment status updates.
		void ConnectEquipmentHub(const std::shared_ptr<Kernel::EquipmentHub>& equipment_hub);

		/// Connect to the statistics hub to receive message and bandwidth stats.
		void ConnectStatisticsHub(const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub);

		//---------------------------------------------------------------------
		// COMMAND HANDLING
		//---------------------------------------------------------------------

		/// Register a handler for a specific command.
		void RegisterCommand(const std::string& command, CommandHandler handler);

		/// Unregister a command handler.
		void UnregisterCommand(const std::string& command);

		/// Check if a handler is registered for a specific command.
		bool HasCommand(const std::string& command) const;

		/// Get the number of registered command handlers.
		std::size_t CommandCount() const noexcept;

		//---------------------------------------------------------------------
		// MANUAL PUBLISHING
		//---------------------------------------------------------------------

		/// Publish all current status immediately.
		void PublishAllStatus();

		/// Publish a custom JSON payload to a subtopic.
		void PublishCustom(const std::string& subtopic, const nlohmann::json& payload);

		/// Get the underlying MQTT client (for use by HA discovery and similar layers).
		std::shared_ptr<MqttClient> GetMqttClient() const { return m_Client; }

		/// Signal fired after device status is published. Used by HA discovery to update device states.
		boost::signals2::signal<void()> OnDevicesPublished;

		//---------------------------------------------------------------------
		// TOPIC UTILITIES
		//---------------------------------------------------------------------

		std::string StatusTopic(const std::string& subtopic) const;
		std::string EventTopic(const std::string& event_type) const;
		std::string CommandTopic(const std::string& action) const;
		bool IsCommandTopic(const std::string& topic) const;
		std::string ExtractCommand(const std::string& topic) const;

	private:
		//---------------------------------------------------------------------
		// SIGNAL HANDLERS
		//---------------------------------------------------------------------

		void OnDataHubConfigChanged(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event);
		void OnEquipmentStatusChanged(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>& event);

		/// Mark that a hub change occurred so the next Poll() flushes an on-change
		/// publish (debounced). Called from the hub-change signal handlers, which fire
		/// on the protocol-decode hot path; the actual publish is deferred to Poll().
		void RequestOnChangePublish();

		//---------------------------------------------------------------------
		// PUBLISHING METHODS
		//---------------------------------------------------------------------

		void PublishStaticTopics();
		void PublishSystemStatus();
		void PublishPoolStatus();
		void PublishDeviceStatus();
		void PublishStatistics();

		//---------------------------------------------------------------------
		// RETAINED-TOPIC RECONCILIATION
		//---------------------------------------------------------------------

		/// The full set of per-device retained topics the current device set owns: the JSON
		/// topic for every device plus (when HA is enabled) its HA short-state topic. Computed
		/// with the same Slugify + TopicScheme the publishers use, so it matches them exactly.
		std::unordered_set<std::string> ComputeOwnedDeviceTopics() const;

		/// One-shot, post-connect sweep: clear (empty retained) every retained device/HA-state
		/// topic the broker is serving that the current device set no longer owns - removing
		/// duplicate/stale topics left by an earlier (buggy or differently-labelled) run.
		void ReconcileRetainedTopics();

		//---------------------------------------------------------------------
		// MESSAGE HANDLING
		//---------------------------------------------------------------------

		void HandleMessage(const std::string& topic, const std::string& payload);
		void ProcessCommand(const std::string& topic, const nlohmann::json& payload);

		//---------------------------------------------------------------------
		// SERIALIZATION HELPERS
		//---------------------------------------------------------------------

		nlohmann::json SerializeSystemStatus() const;
		nlohmann::json SerializeSystemVersion() const;
		nlohmann::json SerializeSystemEquipment() const;
		nlohmann::json SerializeTemperatures() const;
		nlohmann::json SerializeChemistry() const;
		nlohmann::json SerializeCirculation() const;
		nlohmann::json SerializeConfiguration() const;
		nlohmann::json SerializeStatisticsMessages() const;
		nlohmann::json SerializeStatisticsBandwidth() const;
		nlohmann::json SerializeStatisticsLatency() const;
		nlohmann::json SerializeStatisticsSerial() const;
		nlohmann::json SerializeTemperature(const Kernel::Temperature& temp) const { return Utility::SerializeTemperature(temp); }
		nlohmann::json SerializeTemperature(const std::optional<Kernel::Temperature>& temp) const { return Utility::SerializeTemperature(temp); }
		nlohmann::json SerializeTemperature(const std::optional<Kernel::Temperature>& temp, const std::optional<std::chrono::system_clock::time_point>& last_updated, bool stale) const { return Utility::SerializeTemperature(temp, last_updated, stale); }

	private:
		const Options::Mqtt::MqttSettings m_Settings;
		std::shared_ptr<MqttClient> m_Client;

		// Hub connections
		std::weak_ptr<Kernel::DataHub> m_DataHub;
		std::weak_ptr<Kernel::EquipmentHub> m_EquipmentHub;
		std::weak_ptr<Kernel::StatisticsHub> m_StatisticsHub;

		// Signal connections
		boost::signals2::scoped_connection m_DataHubConnection;
		boost::signals2::scoped_connection m_EquipmentHubConnection;
		boost::signals2::scoped_connection m_ClientConnectedConnection;
		boost::signals2::scoped_connection m_ClientMessageConnection;

		// Command handlers
		std::unordered_map<std::string, CommandHandler> m_CommandHandlers;

		// Device JSON topics published on the previous PublishDeviceStatus() sweep. A device
		// that drops out (removed, or relabelled so its slug changes) leaves a retained topic
		// behind; the next sweep clears it with an empty retained payload so consumers (and the
		// broker) do not keep serving a stale/duplicate device. See PublishDeviceStatus().
		std::unordered_set<std::string> m_PublishedDeviceTopics;

		// Startup broker reconciliation: on connect we subscribe to the device/HA-state topic
		// wildcards and collect every retained topic the broker is already serving, then once the
		// grace window elapses (retained delivery is complete) clear any not owned by the current
		// device set. This heals ghosts left by a PRIOR process (which the per-sweep diff above,
		// seeded empty each start, cannot see). Re-armed on every (re)connect.
		struct RetainedReconcileState
		{
			bool Pending{ false };
			std::chrono::steady_clock::time_point Deadline;
			std::unordered_set<std::string> SeenTopics;
		};
		RetainedReconcileState m_RetainedReconcile;
		std::string m_DeviceTopicPrefix;   // "{prefix}/device/"
		std::string m_HaStateTopicPrefix;  // "{prefix}/ha/"
		static constexpr std::chrono::seconds RETAINED_RECONCILE_GRACE{ 10 };

		// State
		bool m_Running{ false };
		bool m_StaticPublished{ false };
		std::chrono::steady_clock::time_point m_StartTime;

		// Monotonic clock for the Poll() deadlines; overridable in tests via
		// SetSteadyClock(). Production uses the real steady_clock.
		SteadyClockFn m_SteadyNow{ [] { return std::chrono::steady_clock::now(); } };

		// Publish scheduling/timing state (using steady_clock comparisons). NextStatusPublish
		// and NextStatsPublish are the periodic publish deadlines; OnChangePending/OnChangeDeadline
		// track the debounced on-change publish: a hub-change signal sets OnChangePending and
		// records the earliest time the deferred publish may fire, and Poll() flushes it.
		struct PublishScheduleState
		{
			std::chrono::steady_clock::time_point NextStatusPublish;
			std::chrono::steady_clock::time_point NextStatsPublish;
			bool OnChangePending{ false };
			std::chrono::steady_clock::time_point OnChangeDeadline;
		};
		PublishScheduleState m_PublishSchedule;

		// Minimum spacing between two on-change publishes, so a burst of hub changes
		// during protocol decode coalesces into a single publish rather than flooding
		// the broker.
		static constexpr std::chrono::milliseconds ON_CHANGE_DEBOUNCE{ 250 };

		// Topic prefixes
		static constexpr const char* STATUS_PREFIX = "status";
		static constexpr const char* EVENT_PREFIX = "event";
		static constexpr const char* COMMAND_PREFIX = "command";

		// Test seam: drive the (signal/timer-gated) retained-topic reconciliation directly.
		friend class AqualinkAutomate::Test::MqttHubReconcileTest;
	};

}
// namespace AqualinkAutomate::Mqtt
