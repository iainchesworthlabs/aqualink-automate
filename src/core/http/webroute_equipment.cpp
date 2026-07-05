#include <chrono>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include "http/json/json_equipment.h"
#include "http/server/make_response.h"
#include "http/webroute_equipment.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "utility/json_serialization_helpers.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// Build the chlorinator sub-object of the chemistry payload from the
		// DataHub chlorinator AuxillaryDevice traits.  Returns a JSON null when
		// no chlorinator (SWG) has been discovered so the UI can render the SWG
		// fields as placeholders rather than fabricating zeroes.
		nlohmann::json BuildChlorinatorJson(const std::shared_ptr<Kernel::DataHub>& data_hub)
		{
			using namespace Kernel::AuxillaryTraitsTypes;

			auto chlorinators = data_hub->Chlorinators();
			if (chlorinators.empty())
			{
				return nlohmann::json{};
			}

			const auto& device = chlorinators.front();
			nlohmann::json chlorinator;

			chlorinator["generating_percent"] = device->AuxillaryTraits.Has(GeneratingPercentageTrait{})
				? nlohmann::json(static_cast<uint8_t>(*(device->AuxillaryTraits[GeneratingPercentageTrait{}])))
				: nlohmann::json{};

			chlorinator["duty_cycle"] = device->AuxillaryTraits.Has(DutyCycleTrait{})
				? nlohmann::json(static_cast<uint8_t>(*(device->AuxillaryTraits[DutyCycleTrait{}])))
				: nlohmann::json{};

			chlorinator["status"] = device->AuxillaryTraits.Has(ChlorinatorStatusTrait{})
				? nlohmann::json(std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[ChlorinatorStatusTrait{}])) })
				: nlohmann::json(std::string{ magic_enum::enum_name(Kernel::ChlorinatorStatuses::Unknown) });

			chlorinator["health"] = device->AuxillaryTraits.Has(ChlorinatorHealthTrait{})
				? nlohmann::json(std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[ChlorinatorHealthTrait{}])) })
				: nlohmann::json(std::string{ magic_enum::enum_name(Kernel::ChlorinatorHealth::Unknown) });

			// Configured Pool / Spa output setpoints (scraped from the Set AquaPure menu),
			// distinct from generating_percent (the instantaneous output, 0 while idle).
			const auto pool_setpoint = device->AuxillaryTraits.Has(ChlorinatorPoolSetpointTrait{})
				? std::optional<uint8_t>(static_cast<uint8_t>(*(device->AuxillaryTraits[ChlorinatorPoolSetpointTrait{}])))
				: std::nullopt;
			const auto spa_setpoint = device->AuxillaryTraits.Has(ChlorinatorSpaSetpointTrait{})
				? std::optional<uint8_t>(static_cast<uint8_t>(*(device->AuxillaryTraits[ChlorinatorSpaSetpointTrait{}])))
				: std::nullopt;
			const auto last_generating = device->AuxillaryTraits.Has(ChlorinatorLastGeneratingTrait{})
				? std::optional<uint8_t>(static_cast<uint8_t>(*(device->AuxillaryTraits[ChlorinatorLastGeneratingTrait{}])))
				: std::nullopt;

			chlorinator["pool_setpoint_percent"] = pool_setpoint.has_value() ? nlohmann::json(pool_setpoint.value()) : nlohmann::json{};
			chlorinator["spa_setpoint_percent"] = spa_setpoint.has_value() ? nlohmann::json(spa_setpoint.value()) : nlohmann::json{};

			// Headline target = the configured setpoint of whichever body is currently active
			// (spa vs pool), falling back to the other body's value, then to the passive
			// last-known generating %, so the dashboard always shows a meaningful target.
			bool spa_active = false;
			for (const auto& body : data_hub->Bodies())
			{
				if (body.IsActive() && body.Id() == Kernel::BodyOfWaterIds::Spa) { spa_active = true; break; }
			}

			std::optional<uint8_t> resolved_setpoint;
			if (spa_active && spa_setpoint.has_value()) { resolved_setpoint = spa_setpoint; }
			else if (!spa_active && pool_setpoint.has_value()) { resolved_setpoint = pool_setpoint; }
			else if (pool_setpoint.has_value()) { resolved_setpoint = pool_setpoint; }   // active body unknown -> prefer pool
			else if (spa_setpoint.has_value()) { resolved_setpoint = spa_setpoint; }
			else { resolved_setpoint = last_generating; }                                // passive fallback

			chlorinator["setpoint_percent"] = resolved_setpoint.has_value() ? nlohmann::json(resolved_setpoint.value()) : nlohmann::json{};

			return chlorinator;
		}
	}
	// namespace

	WebRoute_Equipment::WebRoute_Equipment(Kernel::HubLocator& hub_locator) :
		Interfaces::IWebRoute<EQUIPMENT_ROUTE_URL>(),
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_StatisticsHub(hub_locator.Find<Kernel::StatisticsHub>()),
		m_PreferencesHub(hub_locator.Find<Kernel::PreferencesHub>())
	{
	}

	HTTP::Response WebRoute_Equipment::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment::OnRequest", std::source_location::current());

		nlohmann::json jandy_equipment_json;

		jandy_equipment_json["temperatures"]["pool"] = Utility::SerializeTemperature(m_DataHub->CurrentTempForReporting(Kernel::BodyOfWaterIds::Pool), m_DataHub->PoolTempUpdatedAt(), m_DataHub->PoolTempIsStale());
		jandy_equipment_json["temperatures"]["spa"] = Utility::SerializeTemperature(m_DataHub->CurrentTempForReporting(Kernel::BodyOfWaterIds::Spa), m_DataHub->SpaTempUpdatedAt(), m_DataHub->SpaTempIsStale());
		jandy_equipment_json["temperatures"]["air"] = Utility::SerializeTemperature(m_DataHub->AirTemp(), m_DataHub->AirTempUpdatedAt(), m_DataHub->AirTempIsStale());
		jandy_equipment_json["temperatures"]["pool_setpoint"] = Utility::SerializeTemperature(m_DataHub->PoolTempSetpoint(), m_DataHub->PoolTempSetpointUpdatedAt(), false);
		jandy_equipment_json["temperatures"]["pool_setpoint_2"] = Utility::SerializeTemperature(m_DataHub->PoolTempSetpoint2());
		jandy_equipment_json["temperatures"]["pool_heater_2_enabled"] = m_DataHub->PoolHeater2Enabled().has_value()
			? nlohmann::json(m_DataHub->PoolHeater2Enabled().value()) : nlohmann::json(nullptr);
		jandy_equipment_json["temperatures"]["spa_setpoint"] = Utility::SerializeTemperature(m_DataHub->SpaTempSetpoint(), m_DataHub->SpaTempSetpointUpdatedAt(), false);
		// The server's staleness clock, surfaced so the client can age readings live
		// between fetches (staleness onset generates no WebSocket event) while staying
		// in agreement with the `stale` flags above.
		jandy_equipment_json["temperatures"]["staleness_threshold_seconds"] = m_DataHub->TemperatureStalenessThreshold.count();

		// Nested chemistry payload.  Salt / SWG values come from the DataHub
		// chlorinator AuxillaryDevice traits (+ SaltLevel); ORP / pH read the
		// DataHub chemistry values, which are placeholders today (no sensor on
		// the wire yet) but render automatically once a sensor is decoded.  A
		// reported value of exactly 0 means "no sensor" and is emitted as null
		// so the UI shows a "--" placeholder instead of a misleading zero.
		{
			const auto orp_mv = static_cast<uint16_t>(m_DataHub->ORP()().value());
			// pH is stored as a float32 rounded to 0.1; promoting it raw to double would reintroduce
			// noise (7.1 -> 7.099999904632568), so re-round at the JSON boundary to its real resolution.
			const auto ph_value = Utility::RoundToDecimalPlaces(static_cast<double>(m_DataHub->pH()()), 1);
			const auto salt_ppm = static_cast<uint16_t>(m_DataHub->SaltLevel().value());

			nlohmann::json chemistry;
			chemistry["salt_ppm"] = salt_ppm;
			chemistry["orp_mv"] = (0 == orp_mv) ? nlohmann::json{} : nlohmann::json(orp_mv);
			chemistry["ph"] = (0.0 == ph_value) ? nlohmann::json{} : nlohmann::json(ph_value);
			chemistry["chlorinator"] = BuildChlorinatorJson(m_DataHub);

			jandy_equipment_json["chemistry"] = std::move(chemistry);
		}

		// Configuration section with body-of-water info.
		{
			nlohmann::json config;
			config["pool_configuration"] = std::string{ magic_enum::enum_name(m_DataHub->PoolConfiguration) };
			config["configuration_source"] = std::string{ magic_enum::enum_name(m_DataHub->PoolConfigurationSource) };
			config["expected_auxillary_count"] = static_cast<int>(m_DataHub->ExpectedAuxillaryCount);
			config["expected_power_center_count"] = static_cast<int>(m_DataHub->ExpectedPowerCenterCount);

			// Equipment validation (discovered set vs the model's expected layout). Null until
			// the startup scrape completes; thereafter exposes counts + any anomalies so a
			// short/incomplete scrape or a mis-wired panel is visible to the UI/integrations.
			if (m_DataHub->EquipmentValidationResult.has_value())
			{
				const auto& validation_result = m_DataHub->EquipmentValidationResult.value();
				nlohmann::json validation;
				validation["passed"] = validation_result.Passed();
				validation["expected_auxillaries"] = static_cast<int>(validation_result.ExpectedAuxillaries);
				validation["discovered_auxillaries"] = static_cast<int>(validation_result.DiscoveredAuxillaries);
				validation["expected_power_centers"] = static_cast<int>(validation_result.ExpectedPowerCenters);
				validation["discovered_power_centers"] = static_cast<int>(validation_result.DiscoveredPowerCenters);
				validation["anomalies"] = validation_result.Anomalies;
				config["validation"] = std::move(validation);
			}
			else
			{
				config["validation"] = nlohmann::json{};
			}

			nlohmann::json bodies_array = nlohmann::json::array();
			for (const auto& body : m_DataHub->Bodies())
			{
				// Resolve this body's freshness from the matching temperature channel.
				std::optional<std::chrono::system_clock::time_point> current_updated;
				std::optional<std::chrono::system_clock::time_point> setpoint_updated;
				bool current_stale = false;
				switch (body.Id())
				{
				case Kernel::BodyOfWaterIds::Pool:
					current_updated = m_DataHub->PoolTempUpdatedAt();
					setpoint_updated = m_DataHub->PoolTempSetpointUpdatedAt();
					current_stale = m_DataHub->PoolTempIsStale();
					break;
				case Kernel::BodyOfWaterIds::Spa:
					current_updated = m_DataHub->SpaTempUpdatedAt();
					setpoint_updated = m_DataHub->SpaTempSetpointUpdatedAt();
					current_stale = m_DataHub->SpaTempIsStale();
					break;
				default:
					break;
				}

				nlohmann::json body_json;
				body_json["id"] = std::string{ magic_enum::enum_name(body.Id()) };
				body_json["label"] = body.Label();
				body_json["is_active"] = body.IsActive();
				body_json["temperature"] = Utility::SerializeTemperature(m_DataHub->CurrentTempForReporting(body.Id()), current_updated, current_stale);
				body_json["setpoint"] = Utility::SerializeTemperature(body.TempSetpoint(), setpoint_updated, false);
				bodies_array.push_back(std::move(body_json));
			}
			config["bodies"] = std::move(bodies_array);

			jandy_equipment_json["configuration"] = std::move(config);
		}

		jandy_equipment_json["devices"] = JSON::GenerateJson_Equipment_Devices(m_DataHub, m_PreferencesHub ? m_PreferencesHub->LabelOverrides : nlohmann::json::object(), m_PreferencesHub && m_PreferencesHub->ShowAuxIdInLabel);
		jandy_equipment_json["stats"] = JSON::GenerateJson_Equipment_Stats(m_StatisticsHub);
		jandy_equipment_json["version"] = JSON::GenerateJson_Equipment_Version(m_DataHub);

		auto resp = MakeJsonResponse(req, HTTP::Status::ok, jandy_equipment_json.dump());

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
