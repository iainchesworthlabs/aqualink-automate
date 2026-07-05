#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include "http/webroute_health_detailed.h"
#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_types.h"
#include "interfaces/idevice.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_integration.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "version/version.h"

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		// Process-start reference (static-init). steady_clock is monotonic so uptime
		// is immune to wall-clock steps.
		const auto SERVER_START_TIME = std::chrono::steady_clock::now();

		// Configuration / readiness. Readiness is gated on the pool configuration
		// being determined — the same signal the command routes use to decide whether
		// the system can act — so a not-yet-ready system answers 503 here.
		bool CollectConfiguration(Kernel::HubLocator& hub_locator, nlohmann::json& checks)
		{
			auto data_hub = hub_locator.TryFind<Kernel::DataHub>();
			if (!data_hub)
			{
				checks["configuration"]["status"] = "unavailable";
				return false;
			}

			const bool ready = (data_hub->PoolConfiguration != Kernel::PoolConfigurations::Unknown);

			auto& c = checks["configuration"];
			c["status"] = ready ? "ok" : "pending";
			c["ready"] = ready;
			// User-facing display labels (e.g. "RS-8 Combo", "Dual Body (Shared
			// Equipment)") rather than the raw enum identifiers. This response feeds
			// the diagnostics page, which renders these verbatim; the raw enum name is
			// kept on the machine-token paths (equipment-cache restore/fingerprint and
			// the /api/equipment 'Unknown' readiness check).
			c["pool_configuration"] = Kernel::ToDisplayString(data_hub->PoolConfiguration);
			c["system_board"] = Kernel::ToDisplayString(data_hub->SystemBoard);
			c["mode"] = std::string{ magic_enum::enum_name(data_hub->Mode) };
			c["emulation_disabled"] = data_hub->EmulationDisabled;

			if (data_hub->EquipmentValidationResult.has_value())
			{
				const auto& v = *data_hub->EquipmentValidationResult;
				auto& ev = c["equipment_validation"];
				ev["passed"] = v.Passed();
				ev["expected_auxillaries"] = v.ExpectedAuxillaries;
				ev["discovered_auxillaries"] = v.DiscoveredAuxillaries;
				ev["expected_power_centers"] = v.ExpectedPowerCenters;
				ev["discovered_power_centers"] = v.DiscoveredPowerCenters;
				ev["anomalies"] = v.Anomalies;
			}

			return ready;
		}

		void CollectEquipment(Kernel::HubLocator& hub_locator, nlohmann::json& checks)
		{
			auto equipment_hub = hub_locator.TryFind<Kernel::EquipmentHub>();
			if (!equipment_hub)
			{
				checks["equipment"]["status"] = "unavailable";
				return;
			}

			std::uint32_t device_count{ 0 };
			equipment_hub->ForEachDevice([&device_count](const Interfaces::IDevice&) { ++device_count; });

			auto& e = checks["equipment"];
			e["status"] = (device_count > 0) ? "ok" : "pending";
			e["device_count"] = device_count;
		}

		// MQTT connectivity summary. Intentionally limited to booleans — broker host,
		// credentials, and counters live behind the gated /api/diagnostics/mqtt.
		void CollectMqtt(Kernel::HubLocator& hub_locator, nlohmann::json& checks)
		{
			auto integration = hub_locator.TryFind<Mqtt::MqttIntegration>();
			if (!integration)
			{
				// MQTT not built (disabled) — present but inert, not an error.
				checks["mqtt"]["enabled"] = false;
				return;
			}

			auto& m = checks["mqtt"];
			m["enabled"] = integration->IsEnabled();
			m["running"] = integration->IsRunning();

			auto hub = integration->GetMqttHub();
			auto client = hub ? hub->GetMqttClient() : nullptr;
			if (client)
			{
				m["connected"] = client->IsConnected();
			}
		}
	}

	WebRoute_HealthDetailed::WebRoute_HealthDetailed(Kernel::HubLocator& hub_locator) :
		m_HubLocator(hub_locator)
	{
	}

	HTTP::Response WebRoute_HealthDetailed::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_HealthDetailed::OnRequest", std::source_location::current());

		if (req.method() != HTTP::Verbs::get)
		{
			return HTTP::Responses::Response_405(req);
		}

		const auto uptime_secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - SERVER_START_TIME).count();

		nlohmann::json body;
		body["uptime_seconds"] = uptime_secs;

		// Build identity (safe to expose behind auth; mirrors /api/version).
		body["version"]["name"] = Version::VersionInfo::ProjectName();
		body["version"]["version"] = Version::VersionInfo::ProjectVersionFull();
		if (Version::GitMetadata::Populated())
		{
			body["version"]["commit_sha1"] = Version::GitMetadata::CommitSHA1();
		}

		nlohmann::json checks = nlohmann::json::object();
		const bool ready = CollectConfiguration(m_HubLocator, checks);
		CollectEquipment(m_HubLocator, checks);
		CollectMqtt(m_HubLocator, checks);

		body["ready"] = ready;
		body["status"] = ready ? "ok" : "starting";
		body["checks"] = std::move(checks);

		// Readiness drives the status code so this doubles as a readiness probe:
		// 200 once configured, 503 while still starting.
		const auto code = ready ? HTTP::Status::ok : HTTP::Status::service_unavailable;
		auto resp = MakeJsonResponse(req, code, body.dump());

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
