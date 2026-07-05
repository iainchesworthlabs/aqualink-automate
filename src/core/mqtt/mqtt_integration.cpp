#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <unordered_map>

#include <boost/uuid/string_generator.hpp>

#include <magic_enum/magic_enum.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/circulation.h"
#include "logging/logging.h"
#include "mqtt/mqtt_integration.h"
#include "mqtt/mqtt_payload_parsing.h"
#include "utility/slugify.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Mqtt
{

	namespace
	{
		/// Get current epoch time in seconds for response timestamps.
		int64_t GetTimestampSeconds()
		{
			auto now = std::chrono::system_clock::now();
			return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
		}

		/// Convert CommandResult enum to a string for MQTT responses.
		std::string CommandResultToString(Interfaces::ICommandDispatcher::CommandResult result)
		{
			switch (result)
			{
			case Interfaces::ICommandDispatcher::CommandResult::Success:
				return "success";
			case Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound:
				return "device_not_found";
			case Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter:
				return "no_serial_adapter";
			case Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType:
				return "unknown_equipment_type";
			case Interfaces::ICommandDispatcher::CommandResult::InvalidValue:
				return "invalid_value";
			default:
				return "error";
			}
		}

		/// Build a standard MQTT command response JSON object.
		nlohmann::json BuildCommandResponse(const std::string& command, const std::string& status,
			const nlohmann::json& extra = nlohmann::json::object())
		{
			nlohmann::json response = {
				{"command", command},
				{"status", status},
				{"timestamp", GetTimestampSeconds()}
			};

			for (auto& [key, value] : extra.items())
			{
				response[key] = value;
			}

			return response;
		}

		// Payload parsing/sanitisation helpers live in mqtt_payload_parsing.h so they can be
		// unit-tested directly. Re-expose them unqualified here to keep the call sites concise.
		using PayloadParsing::ParsePayloadNumber;
		using PayloadParsing::ParsePayloadString;
		using PayloadParsing::SanitiseForLog;
	}
	// anonymous namespace

	MqttIntegration::MqttIntegration(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings)
		: m_Settings(settings)
	{
		if (m_Settings.enabled)
		{
			m_Hub = std::make_shared<MqttHub>(io_context, settings);
			RegisterDefaultCommands();

			if (m_Settings.home_assistant_enabled)
			{
				auto client = m_Hub->GetMqttClient();

				// Configure LWT so the broker publishes "offline" on ungraceful disconnect
				auto availability_topic = client->BuildTopic("status/availability");
				client->SetWill(availability_topic, "offline", /*retain=*/true);

				m_HaDiscovery = std::make_shared<HomeAssistantDiscovery>(client, settings);

				LogInfo(Channel::Mqtt, "MQTT Integration initialized with Home Assistant Discovery (enabled)");
			}
			else
			{
				LogInfo(Channel::Mqtt, "MQTT Integration initialized (enabled)");
			}
		}
		else
		{
			LogInfo(Channel::Mqtt, "MQTT Integration initialized (disabled)");
		}
	}

	void MqttIntegration::Start()
	{
		if (!m_Settings.enabled)
		{
			LogInfo(Channel::Mqtt, "MQTT is disabled, skipping start");
			return;
		}

		if (!m_Hub)
		{
			LogError(Channel::Mqtt, "MQTT Hub not initialized");
			return;
		}

		LogInfo(Channel::Mqtt, "Starting MQTT Integration");

		try
		{
			// Connect HA discovery signals before starting the hub (so we catch the first OnConnected)
			if (m_HaDiscovery)
			{
				auto ha = m_HaDiscovery;
				auto client = m_Hub->GetMqttClient();
				m_HaConfigTopic = std::format("{}/device/{}/config", m_Settings.ha_discovery_prefix, m_Settings.ha_device_id);

				// One-shot seed: when the broker replays its retained discovery config, adopt its
				// component list then publish a fresh config - any entity for a device that no longer
				// exists is tombstoned (removed from HA) rather than left as a ghost.
				m_HaConfigSeedConnection = client->OnMessageReceived.connect([this, ha](const std::string& topic, const std::string& payload)
				{
					if (m_HaSeedPending && topic == m_HaConfigTopic)
					{
						ha->AdoptRetainedComponents(payload);
						m_HaSeedPending = false;
						ha->PublishDiscoveryConfigs();
					}
				});

				m_HaConnectedConnection = m_Hub->GetMqttClient()->OnConnected.connect([this, ha]()
				{
					ha->PublishOnline();

					// Defer the first discovery publish until the existing retained config is read
					// back (so removed entities can be tombstoned). Subscribe + arm the grace window.
					m_HaSeedPending = true;
					m_HaSeedDeadline = m_SteadyNow() + HA_SEED_GRACE;
					m_Hub->GetMqttClient()->Subscribe(m_HaConfigTopic, 0);

					ha->PublishDeviceStates();
				});

				m_HaDevicesConnection = m_Hub->OnDevicesPublished.connect([this, ha]()
				{
					// While seeding, hold off republishing the discovery config so we do not overwrite
					// the retained config before reading it back.
					if (!m_HaSeedPending)
					{
						ha->PublishDiscoveryConfigs();
					}
					ha->PublishDeviceStates();
					RegisterDynamicDeviceCommands();
				});
			}

			m_Hub->Start();
			LogInfo(Channel::Mqtt, "MQTT Integration started successfully");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to start MQTT Integration: {}", ex.what()));
		}
	}

	void MqttIntegration::Stop()
	{
		if (!m_Settings.enabled || !m_Hub)
		{
			return;
		}

		LogInfo(Channel::Mqtt, "Stopping MQTT Integration");

		try
		{
			m_HaConnectedConnection.disconnect();
			m_HaDevicesConnection.disconnect();
			m_HaConfigSeedConnection.disconnect();

			m_Hub->Stop();
			LogInfo(Channel::Mqtt, "MQTT Integration stopped");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Error stopping MQTT Integration: {}", ex.what()));
		}
	}

	void MqttIntegration::Poll()
	{
		if (m_Hub)
		{
			m_Hub->Poll();
		}

		// No retained discovery config arrived within the grace window (fresh broker / first run):
		// publish discovery now so HA entities are not held back indefinitely.
		if (m_HaDiscovery && m_HaSeedPending && m_SteadyNow() >= m_HaSeedDeadline)
		{
			m_HaSeedPending = false;
			m_HaDiscovery->PublishDiscoveryConfigs();
		}
	}

	bool MqttIntegration::IsEnabled() const
	{
		return m_Settings.enabled;
	}

	bool MqttIntegration::IsRunning() const
	{
		return m_Hub && m_Hub->IsRunning();
	}

	void MqttIntegration::ConnectHubs(Kernel::HubLocator& hub_locator)
	{
		// Resolve each hub independently (TryFind never throws) so a single missing hub does
		// not abort the whole connect and silently leave the integration unconnected. A missing
		// hub is logged at Error stating which capability is degraded.
		auto data_hub = hub_locator.TryFind<Kernel::DataHub>();
		auto equipment_hub = hub_locator.TryFind<Kernel::EquipmentHub>();
		auto statistics_hub = hub_locator.TryFind<Kernel::StatisticsHub>();
		auto preferences_hub = hub_locator.TryFind<Kernel::PreferencesHub>();

		m_CommandDispatcher = hub_locator.TryFind<Interfaces::ICommandDispatcher>();

		if (!data_hub)
		{
			LogError(Channel::Mqtt, "DataHub not found via locator; MQTT integration degraded (no pool/temperature data published, no device commands)");
		}

		if (!equipment_hub)
		{
			LogError(Channel::Mqtt, "EquipmentHub not found via locator; MQTT integration degraded (no equipment status published)");
		}

		if (!statistics_hub)
		{
			LogError(Channel::Mqtt, "StatisticsHub not found via locator; MQTT integration degraded (no statistics published)");
		}

		if (!m_CommandDispatcher.lock())
		{
			LogError(Channel::Mqtt, "ICommandDispatcher not found via locator; MQTT integration degraded (inbound control commands will be rejected)");
		}

		if (!preferences_hub)
		{
			LogError(Channel::Mqtt, "PreferencesHub not found via locator; MQTT integration degraded (HA setpoint entities stay Celsius regardless of the display-units preference)");
		}

		ConnectHubs(data_hub, equipment_hub, statistics_hub, preferences_hub);
	}

	void MqttIntegration::ConnectHubs(
		const std::shared_ptr<Kernel::DataHub>& data_hub,
		const std::shared_ptr<Kernel::EquipmentHub>& equipment_hub,
		const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub,
		const std::shared_ptr<Kernel::PreferencesHub>& preferences_hub)
	{
		m_DataHub = data_hub;
		m_EquipmentHub = equipment_hub;
		m_StatisticsHub = statistics_hub;
		m_PreferencesHub = preferences_hub;

		if (m_Hub)
		{
			if (data_hub)
			{
				m_Hub->ConnectDataHub(data_hub);
			}

			if (equipment_hub)
			{
				m_Hub->ConnectEquipmentHub(equipment_hub);
			}

			if (statistics_hub)
			{
				m_Hub->ConnectStatisticsHub(statistics_hub);
			}

			if (m_HaDiscovery && data_hub)
			{
				m_HaDiscovery->ConnectDataHub(data_hub);
			}

			if (m_HaDiscovery && preferences_hub)
			{
				m_HaDiscovery->ConnectPreferencesHub(preferences_hub);

				// The setpoint number entities' declared unit/range follow the
				// display-units preference, so a change must republish the
				// (retained) discovery configs for HA to pick it up.
				m_PrefsUnitsConnection = preferences_hub->DisplayUnitsChangedSignal.connect([this]()
				{
					if (m_HaDiscovery)
					{
						LogInfo(Channel::Mqtt, "Temperature display units changed; republishing Home Assistant discovery configs");
						m_HaDiscovery->PublishDiscoveryConfigs();
					}
				});
			}

			// Re-register the device command now that the command dispatcher is available
			RegisterDeviceCommand();
			RegisterSetpointCommand();

			LogDebug(Channel::Mqtt, "MQTT Integration connected to system hubs");
		}
	}

	std::shared_ptr<MqttHub> MqttIntegration::GetMqttHub() const
	{
		return m_Hub;
	}

	void MqttIntegration::RegisterDefaultCommands()
	{
		if (!m_Hub)
		{
			return;
		}

		// Capture a weak_ptr to the hub, NOT a shared_ptr: every handler registered here is
		// stored in the hub's own m_CommandHandlers map, so a shared_ptr capture forms a
		// self-owning cycle (hub -> handler -> hub) that keeps the hub - and its MqttClient -
		// alive to process exit (this was the source of the CRT "Detected memory leaks!" dump).
		// The handler only ever runs while the hub is dispatching it, so lock() always succeeds.
		std::weak_ptr<MqttHub> weak_hub = m_Hub;

		// Register status command - republishes all status
		m_Hub->RegisterCommand("status",
			[weak_hub](const std::string& topic, const nlohmann::json& payload)
			{
				LogDebug(Channel::Mqtt, "Received status command");
				try
				{
					if (auto hub = weak_hub.lock()) { hub->PublishAllStatus(); }
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Mqtt, std::format("Error handling status command: {}", ex.what()));
				}
			});

		// NOTE: the "device" command is intentionally NOT registered here. It is registered in
		// RegisterDeviceCommand() once the command dispatcher is available (after ConnectHubs).
		// The previous placeholder handler here only ever echoed "acknowledged" without acting,
		// and was immediately overwritten by RegisterDeviceCommand(), so it was dead code.

		// Register refresh command - forces status refresh
		m_Hub->RegisterCommand("refresh",
			[weak_hub](const std::string& topic, const nlohmann::json& payload)
			{
				LogDebug(Channel::Mqtt, "Received refresh command");
				try
				{
					if (auto hub = weak_hub.lock())
					{
						hub->PublishAllStatus();

						auto response = BuildCommandResponse("refresh", "completed");
						hub->PublishCustom("response/refresh", response);
					}
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Mqtt, std::format("Error handling refresh command: {}", ex.what()));
				}
			});

		LogDebug(Channel::Mqtt, "Registered default MQTT command handlers");
	}

	void MqttIntegration::RegisterDeviceCommand()
	{
		if (!m_Hub)
		{
			return;
		}

		// weak_ptr capture (see RegisterDefaultCommands): the handler is stored on the hub, so a
		// shared_ptr capture would form a hub -> handler -> hub retain cycle.
		std::weak_ptr<MqttHub> weak_hub = m_Hub;
		std::weak_ptr<Interfaces::ICommandDispatcher> weak_dispatcher = m_CommandDispatcher;

		m_Hub->RegisterCommand("device",
			[weak_hub, weak_dispatcher](const std::string& topic, const nlohmann::json& payload)
			{
				LogDebug(Channel::Mqtt, "Received device command");
				try
				{
					auto dispatcher = weak_dispatcher.lock();
					if (!dispatcher)
					{
						LogWarning(Channel::Mqtt, "Command dispatcher not available");

						auto response = BuildCommandResponse("device", "error", {{"error", "command dispatcher not available"}});
						if (auto hub = weak_hub.lock()) { hub->PublishCustom("response/device", response); }
						return;
					}

					std::string device_id = payload.value("device_id", "");
					std::string action = payload.value("action", "toggle");

					if (device_id.empty())
					{
						LogWarning(Channel::Mqtt, "Device command missing device_id");
						return;
					}

					Interfaces::ICommandDispatcher::CommandResult result = Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound;

					// Try parsing as UUID first, fall back to label
					try
					{
						boost::uuids::string_generator gen;
						auto uuid = gen(device_id);
						result = dispatcher->ToggleByUuid(uuid);
					}
					catch (const std::runtime_error&)
					{
						// Not a valid UUID, try as label
						result = dispatcher->ToggleByLabel(device_id);
					}

					auto response = BuildCommandResponse("device", CommandResultToString(result),
						{{"device_id", device_id}, {"action", action}});
					if (auto hub = weak_hub.lock()) { hub->PublishCustom("response/device", response); }
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Mqtt, std::format("Error handling device command: {}", ex.what()));
				}
			});

		LogDebug(Channel::Mqtt, "Registered device command handler with dispatcher");
	}

	void MqttIntegration::RegisterSetpointCommand()
	{
		if (!m_Hub)
		{
			return;
		}

		// weak_ptr capture (see RegisterDefaultCommands): the handlers are stored on the hub, so a
		// shared_ptr capture would form a hub -> handler -> hub retain cycle.
		std::weak_ptr<MqttHub> weak_hub = m_Hub;
		std::weak_ptr<Interfaces::ICommandDispatcher> weak_dispatcher = m_CommandDispatcher;
		std::weak_ptr<Kernel::DataHub> weak_data_hub = m_DataHub;

		// Helper lambda to convert Celsius to system unit and dispatch a setpoint command.
		// Logs the dispatch outcome for every setpoint command (Warning on failure, Debug on
		// success) so a failed control action is never silently swallowed.
		auto dispatch_setpoint = [weak_dispatcher, weak_data_hub](const std::string& target, double celsius_value) -> std::string
		{
			auto dispatcher = weak_dispatcher.lock();
			if (!dispatcher)
			{
				LogWarning(Channel::Mqtt, std::format("Setpoint command for '{}' could not be dispatched: command dispatcher not available", target));
				return "error";
			}

			if ((target != "pool") && (target != "spa"))
			{
				LogWarning(Channel::Mqtt, std::format("Setpoint command rejected: unknown target '{}'", SanitiseForLog(target)));
				return "invalid_target";
			}

			auto data_hub = weak_data_hub.lock();

			// Convert from Celsius to the system's native unit (typically Fahrenheit), then clamp
			// to the uint8_t wire domain BEFORE the cast so an out-of-range value can never trigger
			// undefined behaviour on the double->uint8_t conversion.
			//
			// NOTE (cross-unit): WU-HTTP-RESPONSE-BUILDER introduces Utility::CelsiusToWireSetpoint
			// in a new src/core/utility/temperature_conversion.h. That header is owned by another
			// work unit and is not yet present, so the conversion + clamp are implemented defensively
			// inline here (mirroring webroute_equipment_setpoints.cpp) and should be routed through
			// the shared helper once it lands.
			const bool is_celsius = (data_hub && (data_hub->SystemTemperatureUnits() == Kernel::TemperatureUnits::Celsius));
			double wire_value = is_celsius
				? std::round(celsius_value)
				: std::round(celsius_value * 9.0 / 5.0 + 32.0);

			wire_value = std::isfinite(wire_value) ? std::clamp(wire_value, 0.0, 255.0) : 0.0;
			const auto temp_value = static_cast<uint8_t>(wire_value);

			const auto result = (target == "pool")
				? dispatcher->SetPoolSetpoint(temp_value)
				: dispatcher->SetSpaSetpoint(temp_value);

			const bool succeeded = (result == Interfaces::ICommandDispatcher::CommandResult::Success);
			const auto wire_setpoint = static_cast<unsigned int>(temp_value);
			const std::string_view result_name = magic_enum::enum_name(result);

			if (succeeded)
			{
				LogDebug(Channel::Mqtt, std::format("Setpoint command for '{}': {}C -> {} (units={}), result={}",
					target, celsius_value, wire_setpoint, is_celsius ? "C" : "F", result_name));
			}
			else
			{
				LogWarning(Channel::Mqtt, std::format("Setpoint command for '{}' failed: {}C -> {} (units={}), result={}",
					target, celsius_value, wire_setpoint, is_celsius ? "C" : "F", result_name));
			}

			return CommandResultToString(result);
		};

		// JSON command handler: {"target": "pool"|"spa", "temperature": <celsius>}
		m_Hub->RegisterCommand("setpoint",
			[weak_hub, dispatch_setpoint](const std::string& topic, const nlohmann::json& payload)
			{
				LogDebug(Channel::Mqtt, "Received setpoint command");
				try
				{
					std::string target = payload.value("target", "");
					double temperature = payload.value("temperature", 0.0);

					if (target.empty() || temperature <= 0.0)
					{
						LogWarning(Channel::Mqtt, "Setpoint command missing target or temperature");
						return;
					}

					auto status_str = dispatch_setpoint(target, temperature);

					auto response = BuildCommandResponse("setpoint", status_str,
						{{"target", target}, {"temperature", temperature}});
					if (auto hub = weak_hub.lock()) { hub->PublishCustom("response/setpoint", response); }
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Mqtt, std::format("Error handling setpoint command: {}", ex.what()));
				}
			});

		// HA number entity handlers: plain text number on setpoint/pool and setpoint/spa.
		// Both capture dispatch_setpoint's result so the outcome is published and never discarded
		// (dispatch_setpoint itself also logs the CommandResult, escalating to Warning on failure).
		//
		// The incoming value arrives in whatever unit the discovery config declared for the
		// number entity — the Temperature_DisplayUnits preference (see AddSetpointComponents) —
		// so it is normalised to the Celsius that dispatch_setpoint expects. Read at command
		// time, matching the discovery config that a preference change republishes.
		std::weak_ptr<Kernel::PreferencesHub> weak_prefs = m_PreferencesHub;
		auto make_ha_setpoint_handler = [weak_hub, dispatch_setpoint, weak_prefs](const std::string& target)
		{
			return [weak_hub, dispatch_setpoint, weak_prefs, target](const std::string& topic, const nlohmann::json& payload)
			{
				LogDebug(Channel::Mqtt, std::format("Received HA {} setpoint command", target));
				try
				{
					auto temperature = ParsePayloadNumber<double>(payload, 0.0);
					if (!(temperature > 0.0))
					{
						LogWarning(Channel::Mqtt, std::format("HA {} setpoint command rejected: missing or out-of-range value '{}'",
							target, SanitiseForLog(ParsePayloadString(payload))));
						return;
					}

					auto celsius = temperature;
					if (auto prefs = weak_prefs.lock(); prefs && (prefs->Temperature_DisplayUnits == Kernel::TemperatureUnits::Fahrenheit))
					{
						celsius = (temperature - 32.0) * 5.0 / 9.0;
					}

					auto status_str = dispatch_setpoint(target, celsius);

					auto response = BuildCommandResponse("setpoint", status_str,
						{{"target", target}, {"temperature", temperature}});
					if (auto hub = weak_hub.lock()) { hub->PublishCustom("response/setpoint", response); }
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Mqtt, std::format("Error handling HA {} setpoint command: {}", target, ex.what()));
				}
			};
		};

		m_Hub->RegisterCommand("setpoint/pool", make_ha_setpoint_handler("pool"));
		m_Hub->RegisterCommand("setpoint/spa", make_ha_setpoint_handler("spa"));

		LogDebug(Channel::Mqtt, "Registered setpoint command handlers");
	}

	void MqttIntegration::RegisterDynamicDeviceCommands()
	{
		if (!m_Hub)
		{
			return;
		}

		auto data_hub = m_DataHub.lock();
		if (!data_hub)
		{
			return;
		}

		std::weak_ptr<Interfaces::ICommandDispatcher> weak_dispatcher = m_CommandDispatcher;

		// Track which device label claimed each command slug this pass so a collision
		// (two distinct labels that Slugify to the same key) is detected rather than
		// silently shadowing the second device.
		std::unordered_map<std::string, std::string> claimed_slugs;

		auto register_device = [&](const std::shared_ptr<Kernel::AuxillaryDevice>& dev)
		{
			if (!dev)
			{
				return;
			}

			auto label_opt = dev->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			if (!label_opt.has_value())
			{
				return;
			}

			auto label = label_opt.value();
			auto slug = Utility::Slugify(label);
			auto command_key = std::format("device/{}", slug);

			// Slug-collision guard: two distinct device labels that Slugify to the same
			// key share one MQTT/HA command topic. The first device to claim it wins;
			// a later colliding device would otherwise be SILENTLY uncontrollable. Warn
			// (naming both) so the operator can rename one, instead of a confusing
			// silent misroute where the topic only ever drives the first device.
			if (auto it = claimed_slugs.find(command_key); it != claimed_slugs.end())
			{
				if (it->second != label)
				{
					LogWarning(Channel::Mqtt, std::format(
						"MQTT device-command slug collision: labels '{}' and '{}' both map to topic 'command/{}'; only '{}' is controllable via that topic. Rename one device.",
						it->second, label, command_key, it->second));
				}
				return;
			}
			claimed_slugs[command_key] = label;

			// Skip if already registered
			if (m_Hub->HasCommand(command_key))
			{
				return;
			}

			m_Hub->RegisterCommand(command_key,
				[weak_dispatcher, label](const std::string& topic, const nlohmann::json& payload)
				{
					LogDebug(Channel::Mqtt, std::format("Received device command for '{}'", label));
					try
					{
						auto dispatcher = weak_dispatcher.lock();
						if (!dispatcher)
						{
							LogWarning(Channel::Mqtt, "Command dispatcher not available for device command");
							return;
						}

						// Parse payload: expect "ON" or "OFF" (plain text from HA switch)
						auto action_str = ParsePayloadString(payload);

						Interfaces::ICommandDispatcher::DeviceAction action = Interfaces::ICommandDispatcher::DeviceAction::Toggle;
						if (action_str == "ON")
						{
							action = Interfaces::ICommandDispatcher::DeviceAction::On;
						}
						else if (action_str == "OFF")
						{
							action = Interfaces::ICommandDispatcher::DeviceAction::Off;
						}
						else
						{
							LogWarning(Channel::Mqtt, std::format("Unknown device action payload: '{}'", SanitiseForLog(action_str)));
							return;
						}

						auto result = dispatcher->CommandByLabel(label, action);

						LogDebug(Channel::Mqtt, std::format("Device command for '{}': action={}, result={}",
							label, action_str, static_cast<int>(result)));
					}
					catch (const std::exception& ex)
					{
						LogError(Channel::Mqtt, std::format("Error handling device command for '{}': {}", label, ex.what()));
					}
				});
		};

		for (const auto& dev : data_hub->Pumps())
		{
			register_device(dev);
		}

		for (const auto& dev : data_hub->Chlorinators())
		{
			register_device(dev);
		}

		for (const auto& dev : data_hub->Auxillaries())
		{
			register_device(dev);
		}

		// Heater enable/disable handlers. Heaters are not actuated through the generic
		// device/{slug} -> CommandByLabel/DeviceActuator path (that drives aux relays / pumps);
		// they get a dedicated heater/{slug} -> SetHeaterMode(body, on/off) route. The heater's
		// body of water (Pool/Spa, or Shared for solar) is captured at registration from its
		// BodyOfWaterTrait so the handler maps straight to the right RSSA heater command.
		auto register_heater = [&](const std::shared_ptr<Kernel::AuxillaryDevice>& dev)
		{
			if (!dev)
			{
				return;
			}

			auto label_opt = dev->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			auto body_opt = dev->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{});
			if (!label_opt.has_value() || !body_opt.has_value())
			{
				LogDebug(Channel::Mqtt, "Skipping heater command registration for heater with no label/body trait");
				return;
			}

			auto label = label_opt.value();
			auto body_id = body_opt.value();
			auto command_key = std::format("heater/{}", Utility::Slugify(label));

			if (m_Hub->HasCommand(command_key))
			{
				return;
			}

			m_Hub->RegisterCommand(command_key,
				[weak_dispatcher, label, body_id](const std::string& topic, const nlohmann::json& payload)
				{
					LogDebug(Channel::Mqtt, std::format("Received heater command for '{}'", label));
					try
					{
						auto dispatcher = weak_dispatcher.lock();
						if (!dispatcher)
						{
							LogWarning(Channel::Mqtt, "Command dispatcher not available for heater command");
							return;
						}

						auto action_str = ParsePayloadString(payload);

						bool enable = false;
						if (action_str == "ON")
						{
							enable = true;
						}
						else if (action_str == "OFF")
						{
							enable = false;
						}
						else
						{
							LogWarning(Channel::Mqtt, std::format("Unknown heater action payload: '{}'", SanitiseForLog(action_str)));
							return;
						}

						auto result = dispatcher->SetHeaterMode(body_id, enable);

						LogDebug(Channel::Mqtt, std::format("Heater command for '{}': action={}, result={}",
							label, action_str, static_cast<int>(result)));
					}
					catch (const std::exception& ex)
					{
						LogError(Channel::Mqtt, std::format("Error handling heater command for '{}': {}", label, ex.what()));
					}
				});
		};

		for (const auto& dev : data_hub->Heaters())
		{
			register_heater(dev);
		}

		// Chlorinator-specific command handlers
		if (!m_Hub->HasCommand("chlorinator/percentage"))
		{
			m_Hub->RegisterCommand("chlorinator/percentage",
				[weak_dispatcher](const std::string& topic, const nlohmann::json& payload)
				{
					LogDebug(Channel::Mqtt, "Received chlorinator percentage command");
					try
					{
						auto dispatcher = weak_dispatcher.lock();
						if (!dispatcher)
						{
							LogWarning(Channel::Mqtt, "Command dispatcher not available for chlorinator percentage");
							return;
						}

						auto percentage = ParsePayloadNumber<uint8_t>(payload);

						auto result = dispatcher->SetChlorinatorPercentage(percentage);
						LogDebug(Channel::Mqtt, std::format("Chlorinator percentage command: {}%, result={}", static_cast<unsigned int>(percentage), static_cast<int>(result)));
					}
					catch (const std::exception& ex)
					{
						LogError(Channel::Mqtt, std::format("Error handling chlorinator percentage command: {}", ex.what()));
					}
				});
		}

		if (!m_Hub->HasCommand("chlorinator/boost"))
		{
			m_Hub->RegisterCommand("chlorinator/boost",
				[weak_dispatcher](const std::string& topic, const nlohmann::json& payload)
				{
					LogDebug(Channel::Mqtt, "Received chlorinator boost command");
					try
					{
						auto dispatcher = weak_dispatcher.lock();
						if (!dispatcher)
						{
							LogWarning(Channel::Mqtt, "Command dispatcher not available for chlorinator boost");
							return;
						}

						auto action_str = ParsePayloadString(payload);
						bool enable = (action_str == "ON");

						auto result = dispatcher->SetChlorinatorBoost(enable);
						LogDebug(Channel::Mqtt, std::format("Chlorinator boost command: {}, result={}", enable ? "ON" : "OFF", static_cast<int>(result)));
					}
					catch (const std::exception& ex)
					{
						LogError(Channel::Mqtt, std::format("Error handling chlorinator boost command: {}", ex.what()));
					}
				});
		}

		// Circulation mode command: accepts "pool", "spa", or "spillover"
		if (!m_Hub->HasCommand("circulation/mode"))
		{
			m_Hub->RegisterCommand("circulation/mode",
				[weak_dispatcher](const std::string& topic, const nlohmann::json& payload)
				{
					LogDebug(Channel::Mqtt, "Received circulation mode command");
					try
					{
						auto dispatcher = weak_dispatcher.lock();
						if (!dispatcher)
						{
							LogWarning(Channel::Mqtt, "Command dispatcher not available for circulation mode");
							return;
						}

						auto mode_str = ParsePayloadString(payload);

						Kernel::CirculationModes mode = Kernel::CirculationModes::Pool;
						if (mode_str == "pool" || mode_str == "Pool")
						{
							mode = Kernel::CirculationModes::Pool;
						}
						else if (mode_str == "spa" || mode_str == "Spa")
						{
							mode = Kernel::CirculationModes::Spa;
						}
						else if (mode_str == "spillover" || mode_str == "Spillover")
						{
							mode = Kernel::CirculationModes::Spillover;
						}
						else
						{
							LogWarning(Channel::Mqtt, std::format("Unknown circulation mode: '{}'", SanitiseForLog(mode_str)));
							return;
						}

						auto result = dispatcher->SetCirculationMode(mode);
						LogDebug(Channel::Mqtt, std::format("Circulation mode command: {}, result={}", mode_str, static_cast<int>(result)));
					}
					catch (const std::exception& ex)
					{
						LogError(Channel::Mqtt, std::format("Error handling circulation mode command: {}", ex.what()));
					}
				});
		}
	}

	std::shared_ptr<MqttIntegration> CreateMqttIntegration(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings)
	{
		if (!settings.enabled)
		{
			LogInfo(Channel::Mqtt, "MQTT is disabled, not creating integration");
			return nullptr;
		}

		return std::make_shared<MqttIntegration>(io_context, settings);
	}

}
// namespace AqualinkAutomate::Mqtt
