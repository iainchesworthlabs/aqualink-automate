#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include "alerting/alert_condition.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "logging/logging.h"
#include "mqtt/ha_discovery.h"
#include "mqtt/mqtt_topic_scheme.h"
#include "utility/slugify.h"
#include "version/version_cmake.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Mqtt
{

	HomeAssistantDiscovery::HomeAssistantDiscovery(std::shared_ptr<MqttClient> client, const Options::Mqtt::MqttSettings& settings)
		: m_Client(std::move(client))
		, m_Settings(settings)
	{
		LogInfo(Channel::Mqtt, "Home Assistant Discovery initialized");
	}

	void HomeAssistantDiscovery::ConnectPreferencesHub(const std::shared_ptr<Kernel::PreferencesHub>& preferences_hub)
	{
		m_PreferencesHub = preferences_hub;
	}

	void HomeAssistantDiscovery::ConnectDataHub(const std::shared_ptr<Kernel::DataHub>& data_hub)
	{
		m_DataHub = data_hub;
		LogDebug(Channel::Mqtt, "HA Discovery connected to Data Hub");
	}

	void HomeAssistantDiscovery::PublishDiscoveryConfigs()
	{
		// Guard the whole discovery build/publish: a single bad device (e.g. a JSON
		// build throwing) must not abort discovery for every entity, which would surface
		// in Home Assistant as silent/unavailable entities with no diagnostic trail.
		try
		{
			LogDebug(Channel::Mqtt, "Publishing Home Assistant device discovery payload");

			nlohmann::json payload;
			payload["dev"] = BuildDeviceObject();
			payload["o"] = BuildOriginObject();
			payload["availability"] = BuildAvailability();

			nlohmann::json cmps = nlohmann::json::object();
			AddTemperatureSensorComponents(cmps);
			AddSetpointComponents(cmps);
			AddChemistrySensorComponents(cmps);
			AddCirculationComponents(cmps);
			AddSystemComponents(cmps);
			AddAlertComponents(cmps);
			AddDynamicDeviceComponents(cmps);

			// Tombstone components present last cycle but gone now so Home Assistant removes those
			// entities. Device-based discovery treats a component reduced to just its platform
			// ({"p": platform}) as a removal; this covers a relabelled/removed device (its slug-keyed
			// component vanishes) and config-gated entities (e.g. the Spa sensors on a pool-only
			// system). Record this cycle's live keys first so the tombstones themselves aren't tracked.
			std::unordered_map<std::string, std::string> current_keys;
			for (const auto& [key, component] : cmps.items())
			{
				if (component.contains("p") && component["p"].is_string())
				{
					current_keys.try_emplace(key, component["p"].get<std::string>());
				}
			}
			for (const auto& [key, platform] : m_PublishedComponentKeys)
			{
				if (!current_keys.contains(key))
				{
					cmps[key] = nlohmann::json{ {"p", platform} };
				}
			}

			payload["cmps"] = std::move(cmps);

			auto topic = std::format("{}/device/{}/config",
				m_Settings.ha_discovery_prefix, m_Settings.ha_device_id);
			m_Client->Publish(topic, payload.dump(), /*retain=*/true);

			m_PublishedComponentKeys = std::move(current_keys);

			LogDebug(Channel::Mqtt, "Home Assistant device discovery payload published");
		}
		catch (const std::exception& ex) // NOSONAR(cpp:S1181) — boundary: batch guard invoked from signals2/poll; one bad device must not abort the whole discovery sweep nor escape into the emitter.
		{
			LogError(Channel::Mqtt, [&ex] { return std::format("Failed to publish HA discovery configs: {}", ex.what()); });
		}
	}

	void HomeAssistantDiscovery::PublishOnline()
	{
		m_Client->Publish(AvailabilityTopic(), "online", /*retain=*/true);
		LogDebug(Channel::Mqtt, "Published HA availability: online");
	}

	void HomeAssistantDiscovery::PublishDeviceStates()
	{
		auto data_hub = m_DataHub.lock();
		if (!data_hub)
		{
			return;
		}

		// Guard the whole sweep: a malformed device must not abort state publishing for
		// the remaining devices (which would leave their HA entities stuck unavailable).
		try
		{
			std::size_t device_count = 0;

			// State topics published this cycle; diffed below to clear any that vanished.
			std::unordered_set<std::string> current_state_topics;

			auto publish_device_state = [&](TopicScheme::DeviceCategory category, const std::shared_ptr<Kernel::AuxillaryDevice>& device)
			{
				if (!device)
				{
					return;
				}

				auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
				if (!label.has_value())
				{
					LogDebug(Channel::Mqtt, [&category] { return std::format("Skipping HA state for {} device with no label trait", TopicScheme::CategoryName(category)); });
					return;
				}

				auto slug = Slugify(label.value());
				auto state_topic = m_Client->BuildTopic(TopicScheme::DeviceStateSubtopic(category, slug));
				auto state = std::string(Kernel::AuxillaryTraitsTypes::ConvertStatusToString(device));

				m_Client->Publish(state_topic, state, /*retain=*/true);
				current_state_topics.insert(std::move(state_topic));
				++device_count;
			};

			for (const auto& device : data_hub->Pumps())
			{
				publish_device_state(TopicScheme::DeviceCategory::Pump, device);
			}

			for (const auto& device : data_hub->Heaters())
			{
				publish_device_state(TopicScheme::DeviceCategory::Heater, device);
			}

			for (const auto& device : data_hub->Chlorinators())
			{
				publish_device_state(TopicScheme::DeviceCategory::Chlorinator, device);
			}

			for (const auto& device : data_hub->Auxillaries())
			{
				publish_device_state(TopicScheme::DeviceCategory::Auxillary, device);
			}

			// Clear (empty retained payload) any state topic published last cycle that is gone now,
			// so a removed or relabelled device leaves no stale retained state behind.
			for (const auto& stale_topic : m_PublishedStateTopics)
			{
				if (!current_state_topics.contains(stale_topic))
				{
					m_Client->Publish(stale_topic, "", /*retain=*/true);
					LogDebug(Channel::Mqtt, [&stale_topic] { return std::format("Cleared retained HA state topic '{}'", stale_topic); });
				}
			}
			m_PublishedStateTopics = std::move(current_state_topics);

			LogTrace(Channel::Mqtt, [&device_count] { return std::format("Published HA device states ({} devices)", device_count); });
		}
		catch (const std::exception& ex) // NOSONAR(cpp:S1181) — boundary: batch guard invoked from signals2/poll; one malformed device must not abort the whole state sweep nor escape into the emitter.
		{
			LogError(Channel::Mqtt, [&ex] { return std::format("Failed to publish HA device states: {}", ex.what()); });
		}
	}

	void HomeAssistantDiscovery::AdoptRetainedComponents(const std::string& retained_config_payload)
	{
		if (retained_config_payload.empty())
		{
			return;
		}

		try
		{
			const auto json = nlohmann::json::parse(retained_config_payload);
			if (!json.contains("cmps") || !json["cmps"].is_object())
			{
				return;
			}

			std::size_t adopted = 0;
			for (const auto& [key, component] : json["cmps"].items())
			{
				if (component.is_object() && component.contains("p") && component["p"].is_string())
				{
					m_PublishedComponentKeys.try_emplace(key, component["p"].get<std::string>());
					++adopted;
				}
			}

			LogDebug(Channel::Mqtt, [&adopted] { return std::format("Adopted {} component(s) from the retained HA discovery config", adopted); });
		}
		catch (const nlohmann::json::exception& ex)
		{
			// The retained payload is untrusted broker data: a malformed / wrong-typed
			// document surfaces here as a parse_error / type_error. Narrowed from a
			// generic catch (cpp:S1181) — a json exception is the only realistic failure.
			LogError(Channel::Mqtt, [&ex] { return std::format("Failed to adopt retained HA discovery config: {}", ex.what()); });
		}
	}

	//=========================================================================
	// Component builders
	//=========================================================================

	void HomeAssistantDiscovery::AddTemperatureSensorComponents(nlohmann::json& cmps)
	{
		auto temperatures_topic = TemperaturesTopic();

		// Respect the installed configuration: only emit the Pool temperature when a Pool body
		// exists and the Spa temperature when a Spa body exists, so a pool-only install gets no
		// dead "Spa" entity (and vice-versa for spa-only). Until bodies are known (config not yet
		// detected) fall back to emitting both so nothing is missing during discovery. Air and
		// freeze-protect are system-wide and always emitted.
		auto [has_pool, has_spa] = ResolveBodyPresence();

		struct TempSensor
		{
			const char* name;
			const char* key;
			const char* value_template;
			bool emit;
		};

		// Sensors deliberately stay declared in °C: the payload value IS celsius,
		// and HA converts device_class=temperature SENSORS to its own configured
		// unit system for display. (Number entities are different — see
		// AddSetpointComponents.)
		const TempSensor sensors[] = {
			{ "Pool Temperature",           "pool_temp",           "{{ value_json.pool.celsius if value_json.pool else '' }}",                     has_pool },
			{ "Spa Temperature",            "spa_temp",            "{{ value_json.spa.celsius if value_json.spa else '' }}",                       has_spa },
			{ "Air Temperature",            "air_temp",            "{{ value_json.air.celsius if value_json.air else '' }}",                       true },
			{ "Freeze Protect Temperature", "freeze_protect_temp", "{{ value_json.freeze_protect.celsius if value_json.freeze_protect else '' }}", true },
		};

		for (const auto& sensor : sensors)
		{
			if (!sensor.emit)
			{
				continue;
			}

			cmps[sensor.key] = {
				{"p", "sensor"},
				{"name", sensor.name},
				{"unique_id", UniqueId(sensor.key)},
				{"state_topic", temperatures_topic},
				{"value_template", sensor.value_template},
				{"device_class", "temperature"},
				{"unit_of_measurement", "\u00B0C"},
				{"state_class", "measurement"}
			};
		}

		// Freshness companions for the three live temperatures: a timestamp sensor ("last updated")
		// and a problem binary_sensor ("stale"). The controller only reports temperatures while the
		// filter pump runs (and only for the active body on a combo system), so these let a HA user
		// see when a reading has gone old without the value disappearing.
		struct FreshnessSensor
		{
			const char* name;
			const char* key;      // value_json sub-object
			const char* id_base;  // unique-id / component-key base
			bool emit;
		};

		const std::array<FreshnessSensor, 3> freshness = {{
			{ "Pool Temperature", "pool", "pool_temp", has_pool },
			{ "Spa Temperature",  "spa",  "spa_temp",  has_spa },
			{ "Air Temperature",  "air",  "air_temp",  true },
		}};

		for (const auto& f : freshness)
		{
			if (!f.emit)
			{
				continue;
			}

			cmps[std::format("{}_updated", f.id_base)] = {
				{"p", "sensor"},
				{"name", std::format("{} Last Updated", f.name)},
				{"unique_id", UniqueId(std::format("{}_updated", f.id_base))},
				{"state_topic", temperatures_topic},
				{"value_template", std::format("{{{{ as_datetime(value_json.{0}.last_updated) if (value_json.{0} and value_json.{0}.last_updated) else none }}}}", f.key)},
				{"device_class", "timestamp"},
				{"entity_category", "diagnostic"}
			};

			cmps[std::format("{}_stale", f.id_base)] = {
				{"p", "binary_sensor"},
				{"name", std::format("{} Stale", f.name)},
				{"unique_id", UniqueId(std::format("{}_stale", f.id_base))},
				{"state_topic", temperatures_topic},
				{"value_template", std::format("{{{{ 'true' if (value_json.{0} and value_json.{0}.stale) else 'false' }}}}", f.key)},
				{"payload_on", "true"},
				{"payload_off", "false"},
				{"device_class", "problem"},
				{"entity_category", "diagnostic"}
			};
		}
	}

	std::pair<bool, bool> HomeAssistantDiscovery::ResolveBodyPresence() const
	{
		auto data_hub = m_DataHub.lock();
		if (!data_hub || data_hub->Bodies().empty())
		{
			// Config not yet detected - emit both so nothing is missing during discovery.
			return { true, true };
		}

		return {
			data_hub->GetBody(Kernel::BodyOfWaterIds::Pool).has_value(),
			data_hub->GetBody(Kernel::BodyOfWaterIds::Spa).has_value()
		};
	}

	void HomeAssistantDiscovery::AddSetpointComponents(nlohmann::json& cmps)
	{
		auto temperatures_topic = TemperaturesTopic();

		// Respect the installed configuration (see AddTemperatureSensorComponents).
		auto [has_pool, has_spa] = ResolveBodyPresence();

		struct SetpointEntity
		{
			const char* name;
			const char* key;
			const char* target;
			bool emit;
		};

		const SetpointEntity entities[] = {
			{ "Pool Setpoint", "pool_setpoint", "pool", has_pool },
			{ "Spa Setpoint",  "spa_setpoint",  "spa",  has_spa },
		};

		// The setpoint NUMBER entities follow the user's Temperature_DisplayUnits
		// preference. Unlike sensors (where HA converts the declared unit to its
		// own unit system, so the \u00B0C-declared celsius payload above is correct),
		// HA presents a number entity exactly as declared \u2014 an imperial user
		// would otherwise get a 15\u201341 \u00B0C slider. The published payload always
		// carries both units, so switching only changes the value_template, the
		// declared unit/range, and how the inbound command value is interpreted
		// (see MqttIntegration's HA setpoint handlers). A preference change
		// republishes discovery via PreferencesHub::DisplayUnitsChangedSignal.
		const auto prefs = m_PreferencesHub.lock();
		const bool fahrenheit = prefs && (prefs->Temperature_DisplayUnits == Kernel::TemperatureUnits::Fahrenheit);
		const char* setpoint_unit_field = fahrenheit ? "fahrenheit" : "celsius";
		const char* setpoint_unit_symbol = fahrenheit ? "°F" : "°C";
		const double setpoint_min = fahrenheit ? 59.0 : 15.0;   // 15\u201341 \u00B0C \u2259 59\u2013105.8 \u00B0F
		const double setpoint_max = fahrenheit ? 106.0 : 41.0;
		const double setpoint_step = fahrenheit ? 1.0 : 0.5;

		for (const auto& entity : entities)
		{
			if (!entity.emit)
			{
				continue;
			}

			cmps[entity.key] = {
				{"p", "number"},
				{"name", entity.name},
				{"unique_id", UniqueId(entity.key)},
				{"state_topic", temperatures_topic},
				{"value_template", std::format("{{{{ value_json.{0}.{1} if value_json.{0} else '' }}}}", entity.key, setpoint_unit_field)},
				{"command_topic", SetpointCommandTopic(entity.target)},
				{"min", setpoint_min},
				{"max", setpoint_max},
				{"step", setpoint_step},
				{"unit_of_measurement", setpoint_unit_symbol},
				{"device_class", "temperature"},
				{"mode", "slider"}
			};
		}

		// POOLSP2 / panel "TEMP2" maintenance setpoint -- reported only on single-body systems.
		// Exposed read-only (a sensor, not a writable number) because the POOLSP2 write path is not
		// yet validated on live hardware. The template yields '' when absent, so HA shows it only
		// when the panel reports it.
		cmps["pool_setpoint_2"] = {
			{"p", "sensor"},
			{"name", "Pool Setpoint 2"},
			{"unique_id", UniqueId("pool_setpoint_2")},
			{"state_topic", temperatures_topic},
			{"value_template", "{{ value_json.pool_setpoint_2.celsius if value_json.pool_setpoint_2 else '' }}"},
			{"unit_of_measurement", "°C"},
			{"device_class", "temperature"},
			{"state_class", "measurement"}
		};

		// POOLHT2 -- whether the TEMP2 maintenance heating is enabled. Read-only binary_sensor
		// (capture-gated decode; no command surface). Present only when the panel reports it.
		cmps["pool_heater_2_enabled"] = {
			{"p", "binary_sensor"},
			{"name", "Pool Heater 2 (TEMP2)"},
			{"unique_id", UniqueId("pool_heater_2_enabled")},
			{"state_topic", temperatures_topic},
			{"value_template", "{{ 'true' if value_json.pool_heater_2_enabled else 'false' }}"},
			{"payload_on", "true"},
			{"payload_off", "false"}
		};
	}

	void HomeAssistantDiscovery::AddChemistrySensorComponents(nlohmann::json& cmps)
	{
		auto chemistry_topic = ChemistryTopic();

		cmps["orp"] = {
			{"p", "sensor"},
			{"name", "ORP"},
			{"unique_id", UniqueId("orp")},
			{"state_topic", chemistry_topic},
			{"value_template", "{{ value_json.orp.value_mv }}"},
			{"device_class", "voltage"},
			{"unit_of_measurement", "mV"},
			{"state_class", "measurement"}
		};

		cmps["ph"] = {
			{"p", "sensor"},
			{"name", "pH"},
			{"unique_id", UniqueId("ph")},
			{"state_topic", chemistry_topic},
			{"value_template", "{{ value_json.ph.value }}"},
			{"device_class", "ph"},
			{"state_class", "measurement"}
		};

		cmps["salt_level"] = {
			{"p", "sensor"},
			{"name", "Salt Level"},
			{"unique_id", UniqueId("salt_level")},
			{"state_topic", chemistry_topic},
			{"value_template", "{{ value_json.salt.value_ppm }}"},
			{"unit_of_measurement", "ppm"},
			{"state_class", "measurement"}
		};
	}

	void HomeAssistantDiscovery::AddCirculationComponents(nlohmann::json& cmps)
	{
		auto circulation_topic = CirculationTopic();

		cmps["circulation_mode"] = {
			{"p", "sensor"},
			{"name", "Circulation Mode"},
			{"unique_id", UniqueId("circulation_mode")},
			{"state_topic", circulation_topic},
			{"value_template", "{{ value_json.mode }}"}
		};

		// Controller operating mode (Normal / Service / TimeOut) from the configuration topic.
		// Lets an HA-only user see when the panel is in Service or Timeout (no control possible).
		cmps["equipment_mode"] = {
			{"p", "sensor"},
			{"name", "Equipment Mode"},
			{"unique_id", UniqueId("equipment_mode")},
			{"state_topic", ConfigurationTopic()},
			{"value_template", "{{ value_json.equipment_mode }}"}
		};

		cmps["spa_mode"] = {
			{"p", "binary_sensor"},
			{"name", "Spa Mode"},
			{"unique_id", UniqueId("spa_mode")},
			{"state_topic", circulation_topic},
			{"value_template", "{{ 'true' if value_json.spa_mode else 'false' }}"},
			{"payload_on", "true"},
			{"payload_off", "false"}
		};

		cmps["clean_mode"] = {
			{"p", "binary_sensor"},
			{"name", "Clean Mode"},
			{"unique_id", UniqueId("clean_mode")},
			{"state_topic", circulation_topic},
			{"value_template", "{{ 'true' if value_json.clean_mode else 'false' }}"},
			{"payload_on", "true"},
			{"payload_off", "false"}
		};

		// Writable Spa Mode switch + circulation-mode select — only for combo/dual systems where a
		// Pool AND a Spa body exist (you can switch between them). On a single-body install there is
		// nothing to toggle. The switch is the convenient pool<->spa toggle (on -> spa, off -> pool);
		// the select keeps full Pool/Spa/Spillover control. Both drive the existing
		// command/circulation/mode handler.
		auto data_hub = m_DataHub.lock();
		const bool is_dual_body = data_hub
			&& (data_hub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_SharedEquipment
				|| data_hub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_DualEquipment);

		if (is_dual_body)
		{
			cmps["spa_mode_switch"] = {
				{"p", "switch"},
				{"name", "Spa Mode"},
				{"unique_id", UniqueId("spa_mode_switch")},
				{"state_topic", circulation_topic},
				{"value_template", "{{ 'ON' if value_json.spa_mode else 'OFF' }}"},
				{"command_topic", CirculationCommandTopic()},
				{"payload_on", "spa"},
				{"payload_off", "pool"},
				{"state_on", "ON"},
				{"state_off", "OFF"}
			};

			cmps["circulation_mode_select"] = {
				{"p", "select"},
				{"name", "Circulation Mode"},
				{"unique_id", UniqueId("circulation_mode_select")},
				{"state_topic", circulation_topic},
				{"value_template", "{{ value_json.mode }}"},
				{"command_topic", CirculationCommandTopic()},
				{"options", nlohmann::json::array({"Pool", "Spa", "Spillover"})}
			};
		}
	}

	void HomeAssistantDiscovery::AddSystemComponents(nlohmann::json& cmps)
	{
		auto system_topic = SystemStatusTopic();

		cmps["uptime"] = {
			{"p", "sensor"},
			{"name", "Uptime"},
			{"unique_id", UniqueId("uptime")},
			{"state_topic", system_topic},
			{"value_template", "{{ value_json.uptime_seconds }}"},
			{"device_class", "duration"},
			{"unit_of_measurement", "s"},
			{"state_class", "total_increasing"},
			{"entity_category", "diagnostic"}
		};
	}

	namespace
	{
		/// Read a device's label trait, returning std::nullopt (so the caller skips it)
		/// when the device is null or unlabelled.
		std::optional<std::string> DeviceLabel(const std::shared_ptr<Kernel::AuxillaryDevice>& dev)
		{
			if (!dev)
			{
				return std::nullopt;
			}

			auto label = dev->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			if (!label.has_value())
			{
				return std::nullopt;
			}

			return label.value();
		}
	}

	void HomeAssistantDiscovery::AddDeviceSwitch(nlohmann::json& cmps, TopicScheme::DeviceCategory category,
		const std::shared_ptr<Kernel::AuxillaryDevice>& dev,
		const std::string& state_on, const std::string& state_off)
	{
		auto label = DeviceLabel(dev);
		if (!label.has_value())
		{
			return;
		}

		auto slug = Slugify(label.value());
		auto key = std::format("{}_{}", TopicScheme::CategoryName(category), slug);
		auto state_topic = m_Client->BuildTopic(TopicScheme::DeviceStateSubtopic(category, slug));

		cmps[key] = {
			{"p", "switch"},
			{"name", label.value()},
			{"unique_id", UniqueId(key)},
			{"state_topic", state_topic},
			{"command_topic", DeviceCommandTopic(slug)},
			{"payload_on", "ON"},
			{"payload_off", "OFF"},
			{"state_on", state_on},
			{"state_off", state_off}
		};
	}

	void HomeAssistantDiscovery::AddDynamicDeviceComponents(nlohmann::json& cmps)
	{
		auto data_hub = m_DataHub.lock();
		if (!data_hub)
		{
			return;
		}

		auto add_switch = [&](TopicScheme::DeviceCategory category, const std::shared_ptr<Kernel::AuxillaryDevice>& dev,
			const std::string& state_on, const std::string& state_off)
		{
			AddDeviceSwitch(cmps, category, dev, state_on, state_off);
		};

		auto add_sensor = [&](TopicScheme::DeviceCategory category, const std::shared_ptr<Kernel::AuxillaryDevice>& dev)
		{
			auto label = DeviceLabel(dev);
			if (!label.has_value())
			{
				return;
			}

			auto slug = Slugify(label.value());
			auto key = std::format("{}_{}", TopicScheme::CategoryName(category), slug);
			auto state_topic = m_Client->BuildTopic(TopicScheme::DeviceStateSubtopic(category, slug));

			cmps[key] = {
				{"p", "sensor"},
				{"name", label.value()},
				{"unique_id", UniqueId(key)},
				{"state_topic", state_topic}
			};
		};

		// Pumps -> switch (Running / Off)
		for (const auto& dev : data_hub->Pumps())
		{
			add_switch(TopicScheme::DeviceCategory::Pump, dev, "Running", "Off");
		}

		// Chlorinators -> switch (On / Off) + extra entities reading the JSON state blob.
		for (const auto& dev : data_hub->Chlorinators())
		{
			add_switch(TopicScheme::DeviceCategory::Chlorinator, dev, "On", "Off");
			AddChlorinatorComponents(cmps, dev);
		}

		// Auxiliaries -> switch (On / Off)
		for (const auto& dev : data_hub->Auxillaries())
		{
			add_switch(TopicScheme::DeviceCategory::Auxillary, dev, "On", "Off");
		}

		// Heaters -> a read-only sensor (multi-state: Off/Heating/Enabled) for the detailed status,
		// PLUS a writable enable/disable switch. The switch is "on" whenever the heater is Heating
		// or Enabled (its thermostat is active); the controller decides when to actually fire (and
		// enforces its own preconditions, e.g. spa heat requires spa mode + pump - see the panel's
		// §14.3 restrictions). The underlying wire command is validated live
		// (SerialAdapterDevice::QueueHeaterCommand).
		for (const auto& dev : data_hub->Heaters())
		{
			add_sensor(TopicScheme::DeviceCategory::Heater, dev);

			auto label = DeviceLabel(dev);
			if (!label.has_value())
			{
				continue;
			}

			auto slug = Slugify(label.value());
			auto key = std::format("heater_{}_switch", slug);
			auto state_topic = m_Client->BuildTopic(TopicScheme::DeviceStateSubtopic(TopicScheme::DeviceCategory::Heater, slug));

			cmps[key] = {
				{"p", "switch"},
				{"name", std::format("{} Enable", label.value())},
				{"unique_id", UniqueId(key)},
				{"state_topic", state_topic},
				{"value_template", "{{ 'ON' if value in ['Heating', 'Enabled'] else 'OFF' }}"},
				{"command_topic", HeaterCommandTopic(slug)},
				{"payload_on", "ON"},
				{"payload_off", "OFF"},
				{"state_on", "ON"},
				{"state_off", "OFF"}
			};
		}
	}

	void HomeAssistantDiscovery::AddChlorinatorComponents(nlohmann::json& cmps, const std::shared_ptr<Kernel::AuxillaryDevice>& dev) const
	{
		auto label = DeviceLabel(dev);
		if (!label.has_value())
		{
			return;
		}

		auto slug = Slugify(label.value());

		// The chlorinator's rich attributes ride the full JSON status blob published by
		// MqttHub to the device topic (TopicScheme::DeviceJsonSubtopic), so these entities
		// point there via value_json templates rather than at the short-string ha/ topic.
		auto state_topic = DeviceStateTopic(slug);

		// Read-only sensors: name suffix + the value_json field they extract.
		struct ChlorinatorSensor
		{
			const char* key_suffix;
			const char* name_suffix;
			const char* value_template;
			const char* unit;        // nullptr => no unit_of_measurement
			bool measurement;        // true => state_class "measurement"
		};

		// `generating_reason` says WHY the cell is at its current output. Without it a 0%
		// Generating reading is ambiguous in Home Assistant -- an automation cannot tell a
		// chlorinator that has been switched off from one that is configured and healthy but
		// waiting on the filter pump. `setpoint_percent` is the configured target it is
		// waiting to produce, which "Generating %" (instantaneous) never shows while idle.
		static constexpr std::array<ChlorinatorSensor, 5> sensors = {{
			{ "generating", "Generating %",  "{{ value_json.generating_percentage }}", "%", true },
			{ "setpoint",   "Target %",      "{{ value_json.setpoint_percent }}",      "%", true },
			{ "reason",     "Output State",  "{{ value_json.generating_reason }}",     nullptr, false },
			{ "boost",      "Boost Mode",    "{{ value_json.boost_mode }}",            nullptr, false },
			{ "health",     "Health",        "{{ value_json.chlorinator_health }}",    nullptr, false },
		}};

		for (const auto& sensor : sensors)
		{
			auto key = std::format("chlorinator_{}_{}", slug, sensor.key_suffix);

			nlohmann::json component = {
				{"p", "sensor"},
				{"name", std::format("{} {}", label.value(), sensor.name_suffix)},
				{"unique_id", UniqueId(key)},
				{"state_topic", state_topic},
				{"value_template", sensor.value_template}
			};

			if (sensor.unit != nullptr)
			{
				component["unit_of_measurement"] = sensor.unit;
			}

			if (sensor.measurement)
			{
				component["state_class"] = "measurement";
			}

			cmps[key] = std::move(component);
		}

		// Output setpoints, ONE PER BODY. The panel keeps pool and spa independent -- you can run
		// the spa at 70% and the pool at 40% -- so a single control could only ever drive one of
		// them, silently.
		//
		// The pool entity keeps its original unique_id so existing Home Assistant dashboards and
		// automations survive: the old single "Generating Setpoint" already drove the pool. Its
		// value_template is CORRECTED here though -- it read `generating_percentage`, the
		// INSTANTANEOUS output, so the control snapped back to 0 whenever the cell was idle
		// instead of showing the configured target.
		auto pct_cmd_key = std::format("chlorinator_{}_pct_cmd", slug);
		cmps[pct_cmd_key] = {
			{"p", "number"},
			{"name", std::format("{} Pool Output", label.value())},
			{"unique_id", UniqueId(pct_cmd_key)},
			{"state_topic", state_topic},
			{"value_template", "{{ value_json.pool_setpoint_percent }}"},
			{"command_topic", ChlorinatorCommandTopic("percentage")},
			{"min", 0},
			{"max", 100},
			{"step", 1},
			{"unit_of_measurement", "%"},
			{"mode", "slider"}
		};

		auto spa_pct_cmd_key = std::format("chlorinator_{}_spa_pct_cmd", slug);
		cmps[spa_pct_cmd_key] = {
			{"p", "number"},
			{"name", std::format("{} Spa Output", label.value())},
			{"unique_id", UniqueId(spa_pct_cmd_key)},
			{"state_topic", state_topic},
			{"value_template", "{{ value_json.spa_setpoint_percent }}"},
			{"command_topic", ChlorinatorCommandTopic("spa/percentage")},
			{"min", 0},
			{"max", 100},
			{"step", 1},
			{"unit_of_measurement", "%"},
			{"mode", "slider"}
		};

		// Boost switch (command topic).
		auto boost_cmd_key = std::format("chlorinator_{}_boost_cmd", slug);
		cmps[boost_cmd_key] = {
			{"p", "switch"},
			{"name", std::format("{} Boost", label.value())},
			{"unique_id", UniqueId(boost_cmd_key)},
			{"state_topic", state_topic},
			{"value_template", "{{ value_json.boost_mode }}"},
			{"command_topic", ChlorinatorCommandTopic("boost")},
			{"payload_on", "ON"},
			{"payload_off", "OFF"},
			{"state_on", "Boost"},
			{"state_off", "Off"}
		};
	}

	//=========================================================================
	// Helpers
	//=========================================================================

	nlohmann::json HomeAssistantDiscovery::BuildDeviceObject() const
	{
		nlohmann::json device = {
			{"identifiers", nlohmann::json::array({m_Settings.ha_device_id})},
			{"name", "aqualink-automate"},
			{"manufacturer", "Jandy / Zodiac"},
			{"sw_version", Version::VersionInfo::ProjectVersionFull()}
		};

		if (auto data_hub = m_DataHub.lock())
		{
			if (auto model = data_hub->EquipmentVersions.ModelNumber(); !model.empty())
			{
				device["model"] = model;
			}

			auto firmware = data_hub->EquipmentVersions.FirmwareRevision();
			if (!firmware.empty())
			{
				device["hw_version"] = firmware;
			}
		}

		return device;
	}

	nlohmann::json HomeAssistantDiscovery::BuildOriginObject() const
	{
		return {
			{"name", "aqualink-automate"},
			{"sw_version", Version::VersionInfo::ProjectVersionFull()},
			{"support_url", Version::VersionInfo::ProjectHomepageURL()}
		};
	}

	nlohmann::json HomeAssistantDiscovery::BuildAvailability() const
	{
		return nlohmann::json::array({
			{{"topic", AvailabilityTopic()}, {"payload_available", "online"}, {"payload_not_available", "offline"}}
		});
	}

	std::string HomeAssistantDiscovery::Slugify(const std::string& input)
	{
		return Utility::Slugify(input);
	}

	std::string HomeAssistantDiscovery::UniqueId(const std::string& suffix) const
	{
		return std::format("{}_{}", m_Settings.ha_device_id, suffix);
	}

	std::string HomeAssistantDiscovery::AvailabilityTopic() const
	{
		return m_Client->BuildTopic("status/availability");
	}

	std::string HomeAssistantDiscovery::TemperaturesTopic() const
	{
		return m_Client->BuildTopic("pool/temperatures");
	}

	std::string HomeAssistantDiscovery::ChemistryTopic() const
	{
		return m_Client->BuildTopic("pool/chemistry");
	}

	std::string HomeAssistantDiscovery::CirculationTopic() const
	{
		return m_Client->BuildTopic("pool/circulation");
	}

	std::string HomeAssistantDiscovery::ConfigurationTopic() const
	{
		return m_Client->BuildTopic("pool/configuration");
	}

	std::string HomeAssistantDiscovery::SystemStatusTopic() const
	{
		return m_Client->BuildTopic("system/status");
	}

	std::string HomeAssistantDiscovery::AlertStateTopic() const
	{
		return m_Client->BuildTopic(std::string{ Alerting::AlertStateSubtopic });
	}

	void HomeAssistantDiscovery::AddAlertComponents(nlohmann::json& cmps) const
	{
		// One HA binary_sensor (device_class: problem) per AlertMonitor condition.
		// All read the same consolidated state document via a per-condition
		// value_template; the AlertMonitor's MQTT sink publishes that document to
		// AlertStateTopic() on every transition.
		const auto state_topic = AlertStateTopic();

		for (const auto& condition : Alerting::AlertConditions)
		{
			const std::string key{ condition.key };

			cmps[std::format("alert_{}", key)] = {
				{ "p", "binary_sensor" },
				{ "name", std::string{ condition.friendly_name } },
				{ "unique_id", UniqueId(std::format("alert_{}", key)) },
				{ "state_topic", state_topic },
				{ "value_template", std::format("{{{{ value_json.{} }}}}", key) },
				{ "device_class", "problem" },
				{ "payload_on", "true" },
				{ "payload_off", "false" }
			};
		}
	}

	std::string HomeAssistantDiscovery::SetpointCommandTopic(const std::string& target) const
	{
		return m_Client->BuildTopic(std::format("command/setpoint/{}", target));
	}

	std::string HomeAssistantDiscovery::DeviceCommandTopic(const std::string& slug) const
	{
		return m_Client->BuildTopic(std::format("command/device/{}", slug));
	}

	std::string HomeAssistantDiscovery::DeviceStateTopic(const std::string& slug) const
	{
		// Routed through TopicScheme so this matches the JSON-blob topic MqttHub publishes.
		return m_Client->BuildTopic(TopicScheme::DeviceJsonSubtopic(slug));
	}

	std::string HomeAssistantDiscovery::ChlorinatorCommandTopic(const std::string& command) const
	{
		return m_Client->BuildTopic(std::format("command/chlorinator/{}", command));
	}

	std::string HomeAssistantDiscovery::CirculationCommandTopic() const
	{
		return m_Client->BuildTopic("command/circulation/mode");
	}

	std::string HomeAssistantDiscovery::HeaterCommandTopic(const std::string& slug) const
	{
		return m_Client->BuildTopic(std::format("command/heater/{}", slug));
	}

}
// namespace AqualinkAutomate::Mqtt
