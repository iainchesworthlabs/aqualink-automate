#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

#include "kernel/data_hub.h"
#include "kernel/preferences_hub.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_topic_scheme.h"
#include "options/options_mqtt_options.h"


namespace AqualinkAutomate::Mqtt
{

	/// Publishes Home Assistant MQTT Discovery configuration payloads so that
	/// HA auto-discovers sensors and binary_sensors for the pool controller.
	///
	/// Fixed sensors point HA at granular topics:
	///   {prefix}/pool/temperatures, {prefix}/pool/chemistry,
	///   {prefix}/pool/circulation, {prefix}/system/status.
	/// Dynamic devices (pumps, heaters, etc.) get per-device short-string state topics
	/// at {prefix}/ha/{category}_{slug}; richer entities that need decoded fields (the
	/// chlorinator's generating %, boost, health) read the JSON status blob at
	/// {prefix}/device/{slug}. All these topics are constructed via the shared
	/// Mqtt::TopicScheme so a component's state_topic always matches the publisher.
	class HomeAssistantDiscovery
	{
	public:
		HomeAssistantDiscovery(std::shared_ptr<MqttClient> client, const Options::Mqtt::MqttSettings& settings);

		/// Store a weak reference to the data hub for reading device info.
		void ConnectDataHub(const std::shared_ptr<Kernel::DataHub>& data_hub);

		/// Store a weak reference to the preferences hub. Drives the setpoint
		/// NUMBER entities' unit (see AddSetpointComponents): unlike sensors,
		/// HA does not convert number entities to its own unit system, so they
		/// follow the user's Temperature_DisplayUnits preference instead.
		void ConnectPreferencesHub(const std::shared_ptr<Kernel::PreferencesHub>& preferences_hub);

		/// Publish all discovery configs (fixed sensors + dynamic devices).
		void PublishDiscoveryConfigs();

		/// Publish "online" (retained) to the availability topic.
		void PublishOnline();

		/// Publish current state for each dynamic device to per-device topics.
		void PublishDeviceStates();

		/// Seed the published-component set from an EXISTING retained discovery config (read back
		/// from the broker at startup). Any component it lists that the next PublishDiscoveryConfigs
		/// no longer emits is then tombstoned, so entity ghosts left by a prior run are removed from
		/// Home Assistant. Safe to call before the first publish; ignores malformed payloads.
		void AdoptRetainedComponents(const std::string& retained_config_payload);

		//---------------------------------------------------------------------
		// HELPERS (public for testability)
		//---------------------------------------------------------------------

		/// Build the shared HA "device" object for all entities.
		nlohmann::json BuildDeviceObject() const;

		/// Build the "origin" object.
		nlohmann::json BuildOriginObject() const;

		/// Build the availability array.
		nlohmann::json BuildAvailability() const;

		/// Convert a label like "Filter Pump" to "filter_pump".
		static std::string Slugify(const std::string& input);

		/// Build a unique ID: "aqualink_{prefix}_{suffix}".
		std::string UniqueId(const std::string& suffix) const;

		/// Get the availability topic.
		std::string AvailabilityTopic() const;

		/// Get the pool temperatures topic.
		std::string TemperaturesTopic() const;

		/// Get the pool chemistry topic.
		std::string ChemistryTopic() const;

		/// Get the pool circulation topic.
		std::string CirculationTopic() const;

		/// Get the pool configuration topic (pool type, system board, operating mode, date/time).
		std::string ConfigurationTopic() const;

		/// Get the system status topic.
		std::string SystemStatusTopic() const;

		/// Get the consolidated alert-state topic that the alert binary_sensors read
		/// and that the AlertMonitor's MQTT sink publishes to.
		std::string AlertStateTopic() const;

	private:
		//---------------------------------------------------------------------
		// COMPONENT BUILDERS (populate cmps JSON object)
		//---------------------------------------------------------------------

		void AddTemperatureSensorComponents(nlohmann::json& cmps);
		void AddSetpointComponents(nlohmann::json& cmps);
		void AddChemistrySensorComponents(nlohmann::json& cmps);
		void AddCirculationComponents(nlohmann::json& cmps);
		void AddSystemComponents(nlohmann::json& cmps);

		/// Add one `binary_sensor` (device_class: problem) per AlertMonitor
		/// condition, all reading the consolidated AlertStateTopic().
		void AddAlertComponents(nlohmann::json& cmps) const;

		void AddDynamicDeviceComponents(nlohmann::json& cmps);

		/// Add one writable `switch` entity for a labelled device (pump/chlorinator/auxiliary),
		/// mapping its live state string to state_on/state_off. No-op for null/unlabelled devices.
		void AddDeviceSwitch(nlohmann::json& cmps, TopicScheme::DeviceCategory category,
			const std::shared_ptr<Kernel::AuxillaryDevice>& dev,
			const std::string& state_on, const std::string& state_off);

		/// Add the chlorinator's extra entities (generating %, boost, health, setpoint,
		/// boost switch) that read the device's JSON status blob.
		void AddChlorinatorComponents(nlohmann::json& cmps, const std::shared_ptr<Kernel::AuxillaryDevice>& dev) const;

		std::string SetpointCommandTopic(const std::string& target) const;
		std::string DeviceCommandTopic(const std::string& slug) const;
		std::string DeviceStateTopic(const std::string& slug) const;
		std::string ChlorinatorCommandTopic(const std::string& command) const;
		std::string CirculationCommandTopic() const;
		std::string HeaterCommandTopic(const std::string& slug) const;

		/// Resolve which bodies are installed, for config-respecting entity gating. Returns
		/// {has_pool, has_spa}. Until bodies are known (config not yet detected) BOTH are true so
		/// no entity is missing during discovery; once bodies exist they are respected exactly
		/// (pool-only -> {true,false}, spa-only -> {false,true}, combo/dual -> {true,true}).
		std::pair<bool, bool> ResolveBodyPresence() const;

	private:
		std::shared_ptr<MqttClient> m_Client;
		Options::Mqtt::MqttSettings m_Settings;
		std::weak_ptr<Kernel::DataHub> m_DataHub;
		std::weak_ptr<Kernel::PreferencesHub> m_PreferencesHub;

		// Discovery component keys published last cycle (key -> platform "p"). A component that
		// disappears - a removed/relabelled device, or a config-gated entity (e.g. the Spa sensor
		// on a pool-only system) - is tombstoned in the next bundled config (an empty component
		// {"p": platform}) so Home Assistant drops the entity. See PublishDiscoveryConfigs().
		std::unordered_map<std::string, std::string> m_PublishedComponentKeys;

		// HA per-device state topics published last cycle; cleared with an empty retained payload
		// when a device drops out so its state topic does not linger. See PublishDeviceStates().
		std::unordered_set<std::string> m_PublishedStateTopics;
	};

}
// namespace AqualinkAutomate::Mqtt
