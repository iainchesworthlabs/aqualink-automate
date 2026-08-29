#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "http/json/json_data_hub.h"
#include "logging/logging.h"
#include "logging/log_sanitize.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_topic_scheme.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "utility/json_serialization_helpers.h"
#include "utility/slugify.h"
#include "version/version.h"

using namespace AqualinkAutomate::Logging;
using AqualinkAutomate::Utility::Slugify;
using AqualinkAutomate::Utility::NanosToMicros;
using AqualinkAutomate::Utility::SerializeLatency;

namespace AqualinkAutomate::Mqtt
{

	MqttHub::MqttHub(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings)
		: m_Settings(settings)
		, m_Client(std::make_shared<MqttClient>(io_context, settings))
	{
		LogInfo(Channel::Mqtt, "MQTT Hub initialized");
	}

	MqttHub::~MqttHub()
	{
		// scoped_connection RAII handles disconnect() automatically.
		m_Running = false;
	}

	void MqttHub::Start()
	{
		if (m_Running)
		{
			LogWarning(Channel::Mqtt, "MQTT Hub is already running");
			return;
		}

		LogInfo(Channel::Mqtt, "Starting MQTT Hub");

		m_StartTime = m_SteadyNow();

		// Connect client signals
		m_ClientConnectedConnection = m_Client->OnConnected.connect([this]()
		{
			Factory::ProfilerFactory::Instance().Get()->Message("MQTT connected to broker");
			LogInfo(Channel::Mqtt, "MQTT Hub: Client connected to broker");

			// Reset static-published flag so version/equipment republish on reconnect
			m_StaticPublished = false;

			// Subscribe to command topics so we receive inbound commands from the broker
			auto command_wildcard = m_Client->BuildTopic("command/#");
			m_Client->Subscribe(command_wildcard, 0);

			// Arm startup broker reconciliation: subscribe to the per-device topic namespaces so
			// the broker replays every retained device/HA-state topic it holds. We collect them
			// while the grace window runs, then clear any the current device set no longer owns.
			m_DeviceTopicPrefix = m_Client->BuildTopic("device/");
			m_HaStateTopicPrefix = m_Client->BuildTopic("ha/");
			m_Client->Subscribe(m_DeviceTopicPrefix + "#", 0);
			m_Client->Subscribe(m_HaStateTopicPrefix + "#", 0);
			m_RetainedReconcile.SeenTopics.clear();
			m_RetainedReconcile.Pending = true;
			m_RetainedReconcile.Deadline = m_SteadyNow() + RETAINED_RECONCILE_GRACE;

			// Publish initial status on connect
			PublishAllStatus();
		});

		m_ClientMessageConnection = m_Client->OnMessageReceived.connect(
			[this](const std::string& topic, const std::string& payload)
			{
				HandleMessage(topic, payload);
			});

		// Start the client (sets it to Connecting state)
		m_Client->Start();
		m_Running = true;

		// Initialize periodic publish timers
		auto now = m_SteadyNow();
		m_PublishSchedule.NextStatusPublish = now + m_Settings.status_publish_interval;
		m_PublishSchedule.NextStatsPublish = now + m_Settings.statistics_publish_interval;

		LogInfo(Channel::Mqtt, "MQTT Hub started successfully");
	}

	void MqttHub::Stop()
	{
		if (!m_Running)
		{
			LogWarning(Channel::Mqtt, "MQTT Hub is not running");
			return;
		}

		LogInfo(Channel::Mqtt, "Stopping MQTT Hub");

		m_Running = false;

		m_ClientConnectedConnection.disconnect();
		m_ClientMessageConnection.disconnect();
		m_DataHubConnection.disconnect();
		m_EquipmentHubConnection.disconnect();

		m_Client->Stop();

		LogInfo(Channel::Mqtt, "MQTT Hub stopped");
	}

	void MqttHub::Poll()
	{
		if (!m_Running)
		{
			return;
		}

		// Advance the MQTT client state machine
		m_Client->Poll();

		// Handle periodic publishing only when connected
		if (!m_Client->IsConnected())
		{
			return;
		}

		auto now = m_SteadyNow();

		// One-shot startup broker reconciliation, once retained delivery has had time to complete.
		if (m_RetainedReconcile.Pending && now >= m_RetainedReconcile.Deadline)
		{
			m_RetainedReconcile.Pending = false;
			ReconcileRetainedTopics();
		}

		// On-change publish (debounced). A hub change during protocol decode flags a
		// pending publish; here we flush it once the debounce window has elapsed,
		// coalescing a burst of changes into a single publish.
		if (m_PublishSchedule.OnChangePending && now >= m_PublishSchedule.OnChangeDeadline)
		{
			m_PublishSchedule.OnChangePending = false;
			PublishSystemStatus();
			PublishPoolStatus();
			PublishDeviceStatus();
			// Re-arm the periodic timer so the change-driven publish also satisfies the
			// next scheduled interval (avoids a redundant publish moments later).
			m_PublishSchedule.NextStatusPublish = now + m_Settings.status_publish_interval;
		}

		// Periodic status publish
		if (now >= m_PublishSchedule.NextStatusPublish)
		{
			PublishSystemStatus();
			PublishPoolStatus();
			PublishDeviceStatus();
			m_PublishSchedule.NextStatusPublish = now + m_Settings.status_publish_interval;
		}

		// Periodic statistics publish
		if (now >= m_PublishSchedule.NextStatsPublish)
		{
			PublishStatistics();
			m_PublishSchedule.NextStatsPublish = now + m_Settings.statistics_publish_interval;
		}
	}

	bool MqttHub::IsRunning() const
	{
		return m_Running && m_Client && m_Client->IsConnected();
	}

	void MqttHub::ConnectDataHub(const std::shared_ptr<Kernel::DataHub>& data_hub)
	{
		m_DataHub = data_hub;

		if (auto locked_hub = m_DataHub.lock())
		{
			m_DataHubConnection = locked_hub->ConfigUpdateSignal.connect(
				[this](const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event)
				{
					OnDataHubConfigChanged(event);
				});

			LogDebug(Channel::Mqtt, "MQTT Hub connected to Data Hub");
		}
	}

	void MqttHub::ConnectEquipmentHub(const std::shared_ptr<Kernel::EquipmentHub>& equipment_hub)
	{
		m_EquipmentHub = equipment_hub;

		if (auto locked_hub = m_EquipmentHub.lock())
		{
			m_EquipmentHubConnection = locked_hub->EquipmentStatusChangeSignal.connect(
				[this](const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>& event)
				{
					OnEquipmentStatusChanged(event);
				});

			LogDebug(Channel::Mqtt, "MQTT Hub connected to Equipment Hub");
		}
	}

	void MqttHub::ConnectStatisticsHub(const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub)
	{
		m_StatisticsHub = statistics_hub;
		LogDebug(Channel::Mqtt, "MQTT Hub connected to Statistics Hub");
	}

	void MqttHub::RegisterCommand(const std::string& command, CommandHandler handler)
	{
		m_CommandHandlers[command] = std::move(handler);
		LogDebug(Channel::Mqtt, std::format("Registered MQTT command handler: {}", command));
	}

	void MqttHub::UnregisterCommand(const std::string& command)
	{
		if (m_CommandHandlers.erase(command) > 0)
		{
			LogDebug(Channel::Mqtt, std::format("Unregistered MQTT command handler: {}", command));
		}
	}

	bool MqttHub::HasCommand(const std::string& command) const
	{
		return m_CommandHandlers.find(command) != m_CommandHandlers.end();
	}

	std::size_t MqttHub::CommandCount() const noexcept
	{
		return m_CommandHandlers.size();
	}

	void MqttHub::PublishAllStatus()
	{
		if (!IsRunning())
		{
			return;
		}

		LogDebug(Channel::Mqtt, "Publishing all status to MQTT");

		PublishStaticTopics();
		PublishSystemStatus();
		PublishPoolStatus();
		PublishDeviceStatus();
		PublishStatistics();
	}

	void MqttHub::PublishCustom(const std::string& subtopic, const nlohmann::json& payload)
	{
		if (!IsRunning())
		{
			return;
		}

		m_Client->Publish(m_Client->BuildTopic(subtopic), payload.dump());
	}

	void MqttHub::OnDataHubConfigChanged(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::OnDataHubConfigChanged", std::source_location::current());

		if (!IsRunning() || !event || !m_Settings.publish_on_change)
		{
			return;
		}

		LogTrace(Channel::Mqtt, "Data Hub config changed, scheduling on-change publish");
		RequestOnChangePublish();
	}

	void MqttHub::OnEquipmentStatusChanged(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>& event)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::OnEquipmentStatusChanged", std::source_location::current());

		if (!IsRunning() || !event || !m_Settings.publish_on_change)
		{
			return;
		}

		LogTrace(Channel::Mqtt, "Equipment status changed, scheduling on-change publish");
		RequestOnChangePublish();
	}

	void MqttHub::RequestOnChangePublish()
	{
		// Debounce: the first change in a quiet window arms the deadline; subsequent
		// changes within the window keep the existing (earlier) deadline so a burst
		// coalesces into a single deferred publish flushed by Poll().
		if (!m_PublishSchedule.OnChangePending)
		{
			m_PublishSchedule.OnChangePending = true;
			m_PublishSchedule.OnChangeDeadline = m_SteadyNow() + ON_CHANGE_DEBOUNCE;
		}
	}

	void MqttHub::PublishStaticTopics()
	{
		if (m_StaticPublished)
		{
			return;
		}

		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::PublishStaticTopics", std::source_location::current());

		try
		{
			auto version_payload = SerializeSystemVersion().dump();
			m_Client->Publish(m_Client->BuildTopic("system/version"), version_payload, /*retain=*/true);

			auto equipment_payload = SerializeSystemEquipment().dump();
			m_Client->Publish(m_Client->BuildTopic("system/equipment"), equipment_payload, /*retain=*/true);

			m_StaticPublished = true;
			LogTrace(Channel::Mqtt, "Published static topics (version, equipment)");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to publish static topics: {}", ex.what()));
		}
	}

	void MqttHub::PublishSystemStatus()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::PublishSystemStatus", std::source_location::current());

		try
		{
			auto payload = SerializeSystemStatus().dump();
			zone->Value(payload.size());
			m_Client->Publish(m_Client->BuildTopic("system/status"), payload, /*retain=*/true);
			LogTrace(Channel::Mqtt, "Published system status");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to publish system status: {}", ex.what()));
		}
	}

	void MqttHub::PublishPoolStatus()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::PublishPoolStatus", std::source_location::current());

		try
		{
			auto temperatures_payload = SerializeTemperatures().dump();
			m_Client->Publish(m_Client->BuildTopic("pool/temperatures"), temperatures_payload, /*retain=*/true);

			auto chemistry_payload = SerializeChemistry().dump();
			m_Client->Publish(m_Client->BuildTopic("pool/chemistry"), chemistry_payload, /*retain=*/true);

			auto circulation_payload = SerializeCirculation().dump();
			m_Client->Publish(m_Client->BuildTopic("pool/circulation"), circulation_payload, /*retain=*/true);

			auto configuration_payload = SerializeConfiguration().dump();
			m_Client->Publish(m_Client->BuildTopic("pool/configuration"), configuration_payload, /*retain=*/true);

			// Publish per-body temperature topics when bodies are available.
			if (auto data_hub = m_DataHub.lock())
			{
				for (const auto& body : data_hub->Bodies())
				{
					auto body_name = std::string{ magic_enum::enum_name(body.Id()) };
					std::transform(body_name.begin(), body_name.end(), body_name.begin(),
						[](unsigned char c) { return std::tolower(c); });

					// Resolve freshness for this body's current temperature from the matching channel
					// (PoolTempIsStale already suppresses staleness for a dormant inactive body).
					std::optional<std::chrono::system_clock::time_point> current_updated;
					std::optional<std::chrono::system_clock::time_point> setpoint_updated;
					bool current_stale = false;
					switch (body.Id())
					{
					case Kernel::BodyOfWaterIds::Pool:
						current_updated = data_hub->PoolTempUpdatedAt();
						setpoint_updated = data_hub->PoolTempSetpointUpdatedAt();
						current_stale = data_hub->PoolTempIsStale();
						break;
					case Kernel::BodyOfWaterIds::Spa:
						current_updated = data_hub->SpaTempUpdatedAt();
						setpoint_updated = data_hub->SpaTempSetpointUpdatedAt();
						current_stale = data_hub->SpaTempIsStale();
						break;
					default:
						break;
					}

					nlohmann::json body_temp = {
						{"current", SerializeTemperature(data_hub->CurrentTempForReporting(body.Id()), current_updated, current_stale)},
						{"setpoint", SerializeTemperature(body.TempSetpoint(), setpoint_updated, false)},
						{"is_active", body.IsActive()}
					};

					auto body_payload = body_temp.dump();
					m_Client->Publish(m_Client->BuildTopic(std::format("body/{}/temperature", body_name)), body_payload, /*retain=*/true);
				}
			}

			zone->Value(temperatures_payload.size() + chemistry_payload.size() + circulation_payload.size() + configuration_payload.size());
			LogTrace(Channel::Mqtt, "Published pool status (temperatures, chemistry, circulation, configuration)");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to publish pool status: {}", ex.what()));
		}
	}

	void MqttHub::PublishDeviceStatus()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::PublishDeviceStatus", std::source_location::current());

		try
		{
			std::size_t total_size = 0;
			std::size_t device_count = 0;

			// Topics published this sweep; diffed against the previous sweep below to clear any
			// that vanished (device removed, or relabelled so its slug changed).
			std::unordered_set<std::string> current_topics;

			if (auto data_hub = m_DataHub.lock())
			{
				auto auxillaries = data_hub->Auxillaries();
				auto heaters = data_hub->Heaters();
				auto pumps = data_hub->Pumps();
				auto chlorinators = data_hub->Chlorinators();

				LogDebug(Channel::Mqtt, [&auxillaries, &heaters, &pumps, &chlorinators] { return std::format("Publishing device status: {} auxillaries, {} heaters, {} pumps, {} chlorinators",
					auxillaries.size(), heaters.size(), pumps.size(), chlorinators.size()); });

				for (const auto& device : auxillaries)
				{
					PublishOneDevice(TopicScheme::DeviceCategory::Auxillary, device, total_size, device_count, current_topics);
				}

				for (const auto& device : heaters)
				{
					PublishOneDevice(TopicScheme::DeviceCategory::Heater, device, total_size, device_count, current_topics);
				}

				for (const auto& device : pumps)
				{
					PublishOneDevice(TopicScheme::DeviceCategory::Pump, device, total_size, device_count, current_topics);
				}

				for (const auto& device : chlorinators)
				{
					PublishOneDevice(TopicScheme::DeviceCategory::Chlorinator, device, total_size, device_count, current_topics);
				}
			}

			// Clear (empty retained payload) any device topic published last sweep that is gone now,
			// so a removed or relabelled device does not linger in the broker as a retained ghost.
			for (const auto& stale_topic : m_PublishedDeviceTopics)
			{
				if (!current_topics.contains(stale_topic))
				{
					m_Client->Publish(stale_topic, "", /*retain=*/true);
					LogDebug(Channel::Mqtt, [&stale_topic] { return std::format("Cleared retained device topic '{}'", stale_topic); });
				}
			}
			m_PublishedDeviceTopics = std::move(current_topics);

			zone->Value(total_size);
			LogTrace(Channel::Mqtt, std::format("Published device status ({} devices)", device_count));

			OnDevicesPublished();
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to publish device status: {}", ex.what()));
		}
	}

	void MqttHub::PublishOneDevice(TopicScheme::DeviceCategory category, const std::shared_ptr<Kernel::AuxillaryDevice>& device,
		std::size_t& total_size, std::size_t& device_count, std::unordered_set<std::string>& current_topics)
	{
		if (!device)
		{
			return;
		}

		const std::string type{ TopicScheme::CategoryName(category) };

		auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
		if (!label.has_value())
		{
			LogDebug(Channel::Mqtt, [&type] { return std::format("Skipping {} device with no label trait", type); });
			return;
		}

		auto slug = Slugify(label.value());

		nlohmann::json j;
		Kernel::to_json(j, *device);
		j["type"] = type;

		if (auto body_opt = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}); body_opt.has_value())
		{
			j["body_of_water"] = std::string{ magic_enum::enum_name(body_opt.value()) };
		}

		// A chlorinator's instantaneous output is 0% whenever the cell is idle, which on its own
		// reads as "off or broken". Publish the configured target and the REASON alongside it so
		// an automation (or a Home Assistant card) can tell a chlorinator that is switched off
		// from one that is simply waiting for the filter pump. Derived once, in the DataHub, so
		// MQTT and the web UI cannot disagree about the same cell.
		if (TopicScheme::DeviceCategory::Chlorinator == category)
		{
			if (auto data_hub = m_DataHub.lock(); data_hub)
			{
				const auto setpoint = data_hub->ChlorinatorResolvedSetpoint(*device);
				j["setpoint_percent"] = setpoint.has_value() ? nlohmann::json(setpoint.value()) : nlohmann::json{};
				j["generating_reason"] = std::string{ magic_enum::enum_name(data_hub->ResolveChlorinatorGeneratingReason(*device)) };
			}
		}

		auto payload = j.dump();
		total_size += payload.size();
		auto topic = m_Client->BuildTopic(TopicScheme::DeviceJsonSubtopic(slug));
		m_Client->Publish(topic, payload, /*retain=*/true);
		current_topics.insert(std::move(topic));
		++device_count;
	}

	std::unordered_set<std::string> MqttHub::ComputeOwnedDeviceTopics() const
	{
		std::unordered_set<std::string> owned;

		auto data_hub = m_DataHub.lock();
		if (!data_hub)
		{
			return owned;
		}

		auto add = [&](TopicScheme::DeviceCategory category, const std::shared_ptr<Kernel::AuxillaryDevice>& device)
		{
			if (!device)
			{
				return;
			}
			auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			if (!label.has_value())
			{
				return;
			}
			auto slug = Slugify(label.value());
			owned.insert(m_Client->BuildTopic(TopicScheme::DeviceJsonSubtopic(slug)));
			if (m_Settings.home_assistant_enabled)
			{
				owned.insert(m_Client->BuildTopic(TopicScheme::DeviceStateSubtopic(category, slug)));
			}
		};

		for (const auto& device : data_hub->Auxillaries())   { add(TopicScheme::DeviceCategory::Auxillary, device); }
		for (const auto& device : data_hub->Heaters())       { add(TopicScheme::DeviceCategory::Heater, device); }
		for (const auto& device : data_hub->Pumps())         { add(TopicScheme::DeviceCategory::Pump, device); }
		for (const auto& device : data_hub->Chlorinators())  { add(TopicScheme::DeviceCategory::Chlorinator, device); }

		return owned;
	}

	void MqttHub::ReconcileRetainedTopics()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::ReconcileRetainedTopics", std::source_location::current());

		const auto owned = ComputeOwnedDeviceTopics();

		std::size_t cleared = 0;
		for (const auto& seen_topic : m_RetainedReconcile.SeenTopics)
		{
			if (!owned.contains(seen_topic))
			{
				m_Client->Publish(seen_topic, "", /*retain=*/true);
				LogDebug(Channel::Mqtt, [&seen_topic] { return std::format("Broker reconciliation cleared stale retained topic '{}'", seen_topic); });
				++cleared;
			}
		}

		if (cleared > 0)
		{
			LogInfo(Channel::Mqtt, std::format("Startup broker reconciliation cleared {} stale retained device topic(s)", cleared));
		}

		m_RetainedReconcile.SeenTopics.clear();
	}

	void MqttHub::PublishStatistics()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::PublishStatistics", std::source_location::current());

		try
		{
			auto messages_payload = SerializeStatisticsMessages().dump();
			m_Client->Publish(m_Client->BuildTopic("statistics/messages"), messages_payload);

			auto bandwidth_payload = SerializeStatisticsBandwidth().dump();
			m_Client->Publish(m_Client->BuildTopic("statistics/bandwidth"), bandwidth_payload);

			auto latency_payload = SerializeStatisticsLatency().dump();
			m_Client->Publish(m_Client->BuildTopic("statistics/latency"), latency_payload);

			auto serial_payload = SerializeStatisticsSerial().dump();
			m_Client->Publish(m_Client->BuildTopic("statistics/serial"), serial_payload);

			zone->Value(messages_payload.size() + bandwidth_payload.size() + latency_payload.size() + serial_payload.size());
			LogTrace(Channel::Mqtt, "Published statistics");
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Mqtt, std::format("Failed to publish statistics: {}", ex.what()));
		}
	}

	void MqttHub::HandleMessage(const std::string& topic, const std::string& payload)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::HandleMessage", std::source_location::current());
		zone->Text(topic);
		zone->Value(payload.size());

		// While the startup reconciliation window is open, record retained device/HA-state topics
		// the broker replays so ReconcileRetainedTopics() can clear any the current set no longer owns.
		if (m_RetainedReconcile.Pending && !payload.empty()
			&& (topic.starts_with(m_DeviceTopicPrefix) || topic.starts_with(m_HaStateTopicPrefix)))
		{
			m_RetainedReconcile.SeenTopics.insert(topic);
		}

		if (!IsCommandTopic(topic))
		{
			LogTrace(Channel::Mqtt, std::format("Received non-command message on '{}'", Logging::SanitizeForLog(topic)));
			return;
		}

		std::string command = ExtractCommand(topic);
		LogDebug(Channel::Mqtt, std::format("Received command: {}", command));

		nlohmann::json json_payload;
		if (!payload.empty())
		{
			try
			{
				json_payload = nlohmann::json::parse(payload);
			}
			catch (const nlohmann::json::parse_error& ex)
			{
				LogWarning(Channel::Mqtt, std::format("Failed to parse command payload as JSON: {}", ex.what()));
				json_payload = { {"raw", payload} };
			}
		}

		ProcessCommand(topic, json_payload);
	}

	void MqttHub::ProcessCommand(const std::string& topic, const nlohmann::json& payload)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttHub::ProcessCommand", std::source_location::current());
		zone->Text(topic);

		std::string command = ExtractCommand(topic);

		auto it = m_CommandHandlers.find(command);
		if (it != m_CommandHandlers.end())
		{
			LogDebug(Channel::Mqtt, std::format("Executing command: {}", command));
			it->second(topic, payload);
		}
		else
		{
			LogWarning(Channel::Mqtt, std::format("No handler for command: {}", command));
		}
	}

	//=========================================================================
	// Serialization helpers
	//=========================================================================

	nlohmann::json MqttHub::SerializeSystemStatus() const
	{
		auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
			m_SteadyNow() - m_StartTime);

		return {
			{"online", true},
			{"uptime_seconds", uptime.count()}
		};
	}

	nlohmann::json MqttHub::SerializeSystemVersion() const
	{
		return {
			{"application", Version::VersionInfo::ProjectVersionFull()},
			{"build_date", Version::GitMetadata::CommitDate()}
		};
	}

	nlohmann::json MqttHub::SerializeSystemEquipment() const
	{
		nlohmann::json equipment;

		if (auto data_hub = m_DataHub.lock())
		{
			equipment["model_number"] = data_hub->EquipmentVersions.ModelNumber();
			equipment["firmware_revision"] = data_hub->EquipmentVersions.FirmwareRevision();
		}

		return equipment;
	}

	nlohmann::json MqttHub::SerializeTemperatures() const
	{
		nlohmann::json temps;

		if (auto data_hub = m_DataHub.lock())
		{
			temps["air"] = SerializeTemperature(data_hub->AirTemp(), data_hub->AirTempUpdatedAt(), data_hub->AirTempIsStale());
			// Pool/spa current temps report unavailable (null) for an inactive combo body - see
			// DataHub::CurrentTempForReporting.
			temps["pool"] = SerializeTemperature(data_hub->CurrentTempForReporting(Kernel::BodyOfWaterIds::Pool), data_hub->PoolTempUpdatedAt(), data_hub->PoolTempIsStale());
			temps["spa"] = SerializeTemperature(data_hub->CurrentTempForReporting(Kernel::BodyOfWaterIds::Spa), data_hub->SpaTempUpdatedAt(), data_hub->SpaTempIsStale());
			// Freeze-protect and setpoints are configured values that persist legitimately, so they
			// carry their timestamp but are never flagged stale.
			temps["freeze_protect"] = SerializeTemperature(data_hub->FreezeProtectPoint(), data_hub->FreezeProtectPointUpdatedAt(), false);
			temps["pool_setpoint"] = SerializeTemperature(data_hub->PoolTempSetpoint(), data_hub->PoolTempSetpointUpdatedAt(), false);
			temps["pool_setpoint_2"] = SerializeTemperature(data_hub->PoolTempSetpoint2());
			temps["pool_heater_2_enabled"] = data_hub->PoolHeater2Enabled().has_value()
				? nlohmann::json(data_hub->PoolHeater2Enabled().value()) : nlohmann::json(nullptr);
			temps["spa_setpoint"] = SerializeTemperature(data_hub->SpaTempSetpoint(), data_hub->SpaTempSetpointUpdatedAt(), false);
		}

		return temps;
	}

	nlohmann::json MqttHub::SerializeChemistry() const
	{
		nlohmann::json chemistry;

		if (auto data_hub = m_DataHub.lock())
		{
			chemistry["orp"] = {
				{"value_mv", data_hub->ORP()().value()}
			};
			chemistry["ph"] = {
				// Re-round the float32 pH; a raw promotion to double emits noise (7.1 -> 7.0999999...).
				{"value", Utility::RoundToDecimalPlaces(static_cast<double>(data_hub->pH()()), 1)}
			};
			chemistry["salt"] = {
				{"value_ppm", data_hub->SaltLevel().value()}
			};
		}

		return chemistry;
	}

	nlohmann::json MqttHub::SerializeCirculation() const
	{
		nlohmann::json circulation;

		if (auto data_hub = m_DataHub.lock())
		{
			circulation["mode"] = magic_enum::enum_name(data_hub->CirculationMode);
			circulation["spa_mode"] = data_hub->SpaMode();
			circulation["clean_mode"] = data_hub->InCleanMode;
			circulation["pool_configuration"] = magic_enum::enum_name(data_hub->PoolConfiguration);

			if (auto active_body = data_hub->ActiveBody())
			{
				circulation["active_body"] = magic_enum::enum_name(active_body->get().Id());
			}

			auto timeout_seconds = data_hub->TimeoutRemaining().count();
			if (timeout_seconds > 0)
			{
				circulation["timeout_remaining_seconds"] = timeout_seconds;
			}
		}

		return circulation;
	}

	nlohmann::json MqttHub::SerializeConfiguration() const
	{
		nlohmann::json config;

		if (auto data_hub = m_DataHub.lock())
		{
			config["pool_type"] = magic_enum::enum_name(data_hub->PoolConfiguration);
			config["system_board"] = magic_enum::enum_name(data_hub->SystemBoard);
			// Controller operating mode (Normal / Service / TimeOut). Previously only on the
			// WebSocket; surfaced here so an MQTT/Home-Assistant-only consumer can see when the
			// panel is in Service or Timeout (no control is possible in those modes).
			config["equipment_mode"] = magic_enum::enum_name(data_hub->Mode);
			config["date"] = std::format("{:%Y-%m-%d}", data_hub->Date);
			config["time"] = std::format("{:%H:%M:%S}", data_hub->Time);
		}

		return config;
	}

	nlohmann::json MqttHub::SerializeStatisticsMessages() const
	{
		nlohmann::json messages = nlohmann::json::array();

		if (auto statistics_hub = m_StatisticsHub.lock())
		{
			for ([[maybe_unused]] const auto& [msg_id, counter] : statistics_hub->MessageCounts)
			{
				messages.push_back({
					{"count", counter.Count()}
				});
			}
		}

		return messages;
	}

	nlohmann::json MqttHub::SerializeStatisticsBandwidth() const
	{
		nlohmann::json bandwidth;

		if (auto statistics_hub = m_StatisticsHub.lock())
		{
			// Utilisation() is a fractional percentage; emit at 2 dp so the payload doesn't leak
			// e.g. 33.33333333333333 (matches the HTTP equipment JSON and the {:.2f} log format).
			bandwidth["read"] = {
				{"total_bytes", statistics_hub->BandwidthMetrics.Read.TotalBytes},
				{"utilisation_1sec", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Read.Average_OneSecond.Utilisation(), 2)},
				{"utilisation_30sec", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Read.Average_ThirtySecond.Utilisation(), 2)},
				{"utilisation_5min", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Read.Average_FiveMinute.Utilisation(), 2)}
			};
			bandwidth["write"] = {
				{"total_bytes", statistics_hub->BandwidthMetrics.Write.TotalBytes},
				{"utilisation_1sec", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Write.Average_OneSecond.Utilisation(), 2)},
				{"utilisation_30sec", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Write.Average_ThirtySecond.Utilisation(), 2)},
				{"utilisation_5min", Utility::RoundToDecimalPlaces(statistics_hub->BandwidthMetrics.Write.Average_FiveMinute.Utilisation(), 2)}
			};
		}

		return bandwidth;
	}

	nlohmann::json MqttHub::SerializeStatisticsLatency() const
	{
		nlohmann::json latency;

		if (auto statistics_hub = m_StatisticsHub.lock())
		{
			latency["serial_read"] = SerializeLatency(statistics_hub->LatencyMetrics.ReadLatency);
			latency["serial_write"] = SerializeLatency(statistics_hub->LatencyMetrics.WriteLatency);
			latency["message_processing"] = SerializeLatency(statistics_hub->LatencyMetrics.MessageProcessingLatency);
		}

		return latency;
	}

	nlohmann::json MqttHub::SerializeStatisticsSerial() const
	{
		nlohmann::json serial;

		if (auto statistics_hub = m_StatisticsHub.lock())
		{
			serial["message_error_rate"] = statistics_hub->Serial.MessageErrorRate;
			serial["overflow_count"] = statistics_hub->Serial.SerialOverflowCount;
			serial["underflow_count"] = statistics_hub->Serial.SerialUnderflowCount;
			serial["transmission_failures"] = statistics_hub->Serial.TransmissionFailures;
			serial["write_queue_depth"] = statistics_hub->Serial.SerialWriteQueueDepth;
		}

		return serial;
	}

	std::string MqttHub::StatusTopic(const std::string& subtopic) const
	{
		return m_Client->BuildTopic(std::format("{}/{}", STATUS_PREFIX, subtopic));
	}

	std::string MqttHub::EventTopic(const std::string& event_type) const
	{
		return m_Client->BuildTopic(std::format("{}/{}", EVENT_PREFIX, event_type));
	}

	std::string MqttHub::CommandTopic(const std::string& action) const
	{
		return m_Client->BuildTopic(std::format("{}/{}", COMMAND_PREFIX, action));
	}

	bool MqttHub::IsCommandTopic(const std::string& topic) const
	{
		std::string prefix = m_Client->BuildTopic(std::format("{}/", COMMAND_PREFIX));
		return topic.find(prefix) == 0;
	}

	std::string MqttHub::ExtractCommand(const std::string& topic) const
	{
		if (std::string prefix = m_Client->BuildTopic(std::format("{}/", COMMAND_PREFIX)); topic.find(prefix) == 0)
		{
			return topic.substr(prefix.length());
		}

		return "";
	}

}
// namespace AqualinkAutomate::Mqtt
