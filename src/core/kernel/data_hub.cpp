#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "kernel/hub_events/data_hub_config_event_chemistry.h"
#include "kernel/hub_events/data_hub_config_event_circulation.h"
#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "kernel/hub_events/equipment_hub_system_event_status_change.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "utility/case_insensitive_comparision.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <magic_enum/magic_enum.hpp>
#include <source_location>

namespace AqualinkAutomate::Kernel
{
	
	namespace
	{
		// A nullopt current value (never set) counts as a change so the first reading always emits;
		// otherwise defer to Temperature::operator==.
		bool TemperatureChanged(const std::optional<Kernel::Temperature>& current, const Kernel::Temperature& next)
		{
			return !current.has_value() || (*current != next);
		}
	}

	DataHub::DataHub() = default;

	void DataHub::EmitTemperatureEvent(const std::function<void(DataHub_ConfigEvent_Temperature&)>& populate) const
	{
		// Zone the synchronous signals2 fan-out: this measures the cost of every
		// registered listener (WebSocket push, MQTT publish, etc.) reacting to a
		// temperature update.
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DataHub::EmitTemperatureEvent", std::source_location::current());

		// Signal that a temperature update has occurred.
		auto update_event = std::make_shared<DataHub_ConfigEvent_Temperature>();
		populate(*update_event);
		ConfigUpdateSignal(update_event);
	}

	void DataHub::EmitChemistryEvent(const std::function<void(DataHub_ConfigEvent_Chemistry&)>& populate) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DataHub::EmitChemistryEvent", std::source_location::current());

		// Signal that a chemistry update has occurred.
		auto update_event = std::make_shared<DataHub_ConfigEvent_Chemistry>();
		populate(*update_event);
		ConfigUpdateSignal(update_event);
	}

	void DataHub::EmitCirculationEvent(const std::function<void(DataHub_ConfigEvent_Circulation&)>& populate) const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("DataHub::EmitCirculationEvent", std::source_location::current());

		// Signal that a circulation (mode / active-body) update has occurred.
		auto update_event = std::make_shared<DataHub_ConfigEvent_Circulation>();
		populate(*update_event);
		ConfigUpdateSignal(update_event);
	}

	void DataHub::EmitButtonStateChange(const boost::uuids::uuid& button_id, std::string_view status, std::string_view label)
	{
		// Status processors re-scrape and re-publish on every poll; only fan out a button-state
		// change when this button's (status, label) actually differs from the last published value.
		auto entry = std::make_pair(std::string{ status }, std::string{ label });

		if (auto it = m_LastButtonState.find(button_id); it != m_LastButtonState.end() && it->second == entry)
		{
			return;
		}

		m_LastButtonState[button_id] = std::move(entry);

		auto update_event = std::make_shared<DataHub_ConfigEvent_ButtonStateChange>(button_id, status, label);
		ConfigUpdateSignal(update_event);
	}

	void DataHub::SetCirculationMode(CirculationModes mode)
	{
		const bool spa_active = (CirculationModes::Spa == mode
			|| CirculationModes::SpaFill == mode
			|| CirculationModes::SpaDrain == mode);

		auto pool = GetBody(BodyOfWaterIds::Pool);
		auto spa = GetBody(BodyOfWaterIds::Spa);

		// Capture the pre-update resolved state so the event is only fanned out on a real
		// change (callers may invoke this every status poll).
		const auto prev_mode = CirculationMode;
		const bool prev_pool_active = pool ? pool->get().IsActive() : false;
		const bool prev_spa_active = spa ? spa->get().IsActive() : false;

		CirculationMode = mode;

		// In a dual-body system the active body follows the circulation mode. Single-body
		// installs have a fixed active body (set at configuration) and are left untouched.
		if (pool && spa)
		{
			pool->get().IsActive(!spa_active);
			spa->get().IsActive(spa_active);
		}

		if (const bool changed = (prev_mode != CirculationMode)
			|| (pool && prev_pool_active != pool->get().IsActive())
			|| (spa && prev_spa_active != spa->get().IsActive()); !changed)
		{
			return;
		}

		EmitCirculationEvent([this](DataHub_ConfigEvent_Circulation& update_event)
			{
				update_event.Mode(CirculationMode);
				for (const auto& body : m_Bodies)
				{
					update_event.AddBody(body.Id(), body.IsActive());
				}
			});
	}

	void DataHub::ApplyPoolConfiguration(PoolConfigurations config, ConfigurationSource source, BodyOfWaterIds single_body_kind)
	{
		PoolConfiguration = config;
		PoolConfigurationSource = source;

		if (!m_Bodies.empty())
		{
			return; // Bodies already populated
		}

		switch (config)
		{
		case PoolConfigurations::DualBody_SharedEquipment:
		case PoolConfigurations::DualBody_DualEquipment:
			AddBody(BodyOfWater{ BodyOfWaterIds::Pool, "Pool" });
			AddBody(BodyOfWater{ BodyOfWaterIds::Spa, "Spa" });
			break;
		case PoolConfigurations::SingleBody:
			// Pool-only (default) or spa-only - the single body is whichever the user configured.
			if (single_body_kind == BodyOfWaterIds::Spa)
			{
				AddBody(BodyOfWater{ BodyOfWaterIds::Spa, "Spa" });
			}
			else
			{
				AddBody(BodyOfWater{ BodyOfWaterIds::Pool, "Pool" });
			}
			break;
		default:
			break;
		}

		// The Jandy controller always has exactly one active body. For a single-body system that
		// is necessarily its only body (pool-only -> pool, spa-only -> spa); for a dual-body system
		// the active body follows the current circulation mode (Pool by default).
		if (config == PoolConfigurations::SingleBody)
		{
			if (!m_Bodies.empty())
			{
				m_Bodies.front().IsActive(true);
			}
		}
		else
		{
			bool spa_active = (CirculationMode == CirculationModes::Spa
				|| CirculationMode == CirculationModes::SpaFill
				|| CirculationMode == CirculationModes::SpaDrain);

			if (auto pool = GetBody(BodyOfWaterIds::Pool))
			{
				pool->get().IsActive(!spa_active);
			}

			if (auto spa = GetBody(BodyOfWaterIds::Spa))
			{
				spa->get().IsActive(spa_active);
			}
		}
	}

	void DataHub::AddBody(BodyOfWater body)
	{
		// Avoid duplicates
		for (const auto& existing : m_Bodies)
		{
			if (existing.Id() == body.Id())
			{
				return;
			}
		}

		m_Bodies.push_back(std::move(body));
	}

	std::optional<std::reference_wrapper<BodyOfWater>> DataHub::GetBody(BodyOfWaterIds id)
	{
		for (auto& body : m_Bodies)
		{
			if (body.Id() == id)
			{
				return body;
			}
		}

		return std::nullopt;
	}

	std::optional<std::reference_wrapper<const BodyOfWater>> DataHub::GetBody(BodyOfWaterIds id) const
	{
		for (const auto& body : m_Bodies)
		{
			if (body.Id() == id)
			{
				return body;
			}
		}

		return std::nullopt;
	}

	std::optional<std::reference_wrapper<BodyOfWater>> DataHub::ActiveBody()
	{
		for (auto& body : m_Bodies)
		{
			if (body.IsActive())
			{
				return body;
			}
		}

		return std::nullopt;
	}

	const std::vector<BodyOfWater>& DataHub::Bodies() const
	{
		return m_Bodies;
	}

	std::optional<Kernel::Temperature> DataHub::AirTemp() const
	{
		return m_AirTemp;
	}

	std::optional<Kernel::Temperature> DataHub::PoolTemp() const
	{
		if (auto body = GetBody(BodyOfWaterIds::Pool))
		{
			return body->get().CurrentTemp();
		}

		return m_PoolTemp;
	}

	std::optional<Kernel::Temperature> DataHub::SpaTemp() const
	{
		if (auto body = GetBody(BodyOfWaterIds::Spa))
		{
			return body->get().CurrentTemp();
		}

		return m_SpaTemp;
	}

	std::optional<Kernel::Temperature> DataHub::FreezeProtectPoint() const
	{
		return m_FreezeProtectPoint;
	}

	void DataHub::AirTemp(const Kernel::Temperature& air_temp)
	{
		// The controller re-reports temperatures every poll; only fan out the event on an actual
		// value change. The timestamp is still re-stamped each call so staleness/liveness tracking
		// (and the periodic MQTT/WebSocket heartbeat) continue to work.
		const bool changed = TemperatureChanged(m_AirTemp, air_temp);

		m_AirTemp = air_temp;
		m_AirTempUpdatedAt = std::chrono::system_clock::now();
		Factory::ProfilerFactory::Instance().Get()->PlotValue("Air Temp", air_temp.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&air_temp](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.AirTemp(air_temp);
			});
	}

	void DataHub::PoolTemp(const Kernel::Temperature& pool_temp)
	{
		const bool changed = TemperatureChanged(m_PoolTemp, pool_temp);

		m_PoolTemp = pool_temp;
		m_PoolTempUpdatedAt = std::chrono::system_clock::now();

		if (auto body = GetBody(BodyOfWaterIds::Pool))
		{
			body->get().CurrentTemp(pool_temp);
		}

		Factory::ProfilerFactory::Instance().Get()->PlotValue("Pool Temp", pool_temp.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&pool_temp](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.PoolTemp(pool_temp);
			});
	}

	void DataHub::SpaTemp(const Kernel::Temperature& spa_temp)
	{
		const bool changed = TemperatureChanged(m_SpaTemp, spa_temp);

		m_SpaTemp = spa_temp;
		m_SpaTempUpdatedAt = std::chrono::system_clock::now();

		if (auto body = GetBody(BodyOfWaterIds::Spa))
		{
			body->get().CurrentTemp(spa_temp);
		}

		Factory::ProfilerFactory::Instance().Get()->PlotValue("Spa Temp", spa_temp.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&spa_temp](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.SpaTemp(spa_temp);
			});
	}

	std::optional<Kernel::Temperature> DataHub::PoolTempSetpoint() const
	{
		if (auto body = GetBody(BodyOfWaterIds::Pool))
		{
			return body->get().TempSetpoint();
		}

		return m_PoolTempSetpoint;
	}

	std::optional<Kernel::Temperature> DataHub::PoolTempSetpoint2() const
	{
		// TEMP2 is a second setpoint for the SAME pool body (not a separate body of water), so it
		// is held directly rather than via GetBody(Pool) (which already holds TEMP1).
		return m_PoolTempSetpoint2;
	}

	std::optional<bool> DataHub::PoolHeater2Enabled() const
	{
		return m_PoolHeater2Enabled;
	}

	std::optional<Kernel::Temperature> DataHub::SpaTempSetpoint() const
	{
		if (auto body = GetBody(BodyOfWaterIds::Spa))
		{
			return body->get().TempSetpoint();
		}

		return m_SpaTempSetpoint;
	}

	void DataHub::PoolTempSetpoint(const Kernel::Temperature& pool_temp_setpoint)
	{
		const bool changed = TemperatureChanged(m_PoolTempSetpoint, pool_temp_setpoint);

		m_PoolTempSetpoint = pool_temp_setpoint;
		m_PoolTempSetpointUpdatedAt = std::chrono::system_clock::now();

		if (auto body = GetBody(BodyOfWaterIds::Pool))
		{
			body->get().TempSetpoint(pool_temp_setpoint);
		}

		Factory::ProfilerFactory::Instance().Get()->PlotValue("Pool Temp Setpoint", pool_temp_setpoint.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&pool_temp_setpoint](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.PoolSetpoint(pool_temp_setpoint);
			});
	}

	void DataHub::PoolTempSetpoint2(const Kernel::Temperature& pool_temp_setpoint_2)
	{
		const bool changed = TemperatureChanged(m_PoolTempSetpoint2, pool_temp_setpoint_2);

		m_PoolTempSetpoint2 = pool_temp_setpoint_2;

		Factory::ProfilerFactory::Instance().Get()->PlotValue("Pool Temp Setpoint 2", pool_temp_setpoint_2.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&pool_temp_setpoint_2](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.PoolSetpoint2(pool_temp_setpoint_2);
			});
	}

	void DataHub::PoolHeater2Enabled(bool pool_heater_2_enabled)
	{
		const bool changed = (m_PoolHeater2Enabled != pool_heater_2_enabled);

		m_PoolHeater2Enabled = pool_heater_2_enabled;

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([pool_heater_2_enabled](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.PoolHeater2Enabled(pool_heater_2_enabled);
			});
	}

	void DataHub::SpaTempSetpoint(const Kernel::Temperature& spa_temp_setpoint)
	{
		const bool changed = TemperatureChanged(m_SpaTempSetpoint, spa_temp_setpoint);

		m_SpaTempSetpoint = spa_temp_setpoint;
		m_SpaTempSetpointUpdatedAt = std::chrono::system_clock::now();

		if (auto body = GetBody(BodyOfWaterIds::Spa))
		{
			body->get().TempSetpoint(spa_temp_setpoint);
		}

		Factory::ProfilerFactory::Instance().Get()->PlotValue("Spa Temp Setpoint", spa_temp_setpoint.InCelsius().value());

		if (!changed)
		{
			return;
		}

		EmitTemperatureEvent([&spa_temp_setpoint](DataHub_ConfigEvent_Temperature& update_event)
			{
				update_event.SpaSetpoint(spa_temp_setpoint);
			});
	}

	Kernel::TemperatureUnits DataHub::SystemTemperatureUnits() const
	{
		return m_SystemTemperatureUnits;
	}

	void DataHub::SystemTemperatureUnits(Kernel::TemperatureUnits units)
	{
		m_SystemTemperatureUnits = units;

		// Deliberately does NOT emit a ConfigUpdateSignal: DataHub_ConfigEvent_Temperature
		// carries no temperature-units field, so an emitted event would be an empty
		// ({}) no-op for WebSocket/MQTT consumers. If a units change ever needs to be
		// surfaced, add a units field to the temperature event (owned by the hub-events
		// unit) and emit it here via EmitTemperatureEvent.
	}

	void DataHub::FreezeProtectPoint(const Kernel::Temperature& freeze_protect_point)
	{
		m_FreezeProtectPoint = freeze_protect_point;
		m_FreezeProtectPointUpdatedAt = std::chrono::system_clock::now();

		// Deliberately does NOT emit a ConfigUpdateSignal: DataHub_ConfigEvent_Temperature
		// carries no freeze-protect field, so an emitted event would be an empty ({})
		// no-op for WebSocket/MQTT consumers. If the freeze-protect setpoint ever needs
		// to be surfaced, add a field to the temperature event (owned by the hub-events
		// unit) and emit it here via EmitTemperatureEvent.
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::AirTempUpdatedAt() const
	{
		return m_AirTempUpdatedAt;
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::PoolTempUpdatedAt() const
	{
		return m_PoolTempUpdatedAt;
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::SpaTempUpdatedAt() const
	{
		return m_SpaTempUpdatedAt;
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::PoolTempSetpointUpdatedAt() const
	{
		return m_PoolTempSetpointUpdatedAt;
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::SpaTempSetpointUpdatedAt() const
	{
		return m_SpaTempSetpointUpdatedAt;
	}

	std::optional<std::chrono::system_clock::time_point> DataHub::FreezeProtectPointUpdatedAt() const
	{
		return m_FreezeProtectPointUpdatedAt;
	}

	bool DataHub::IsStale(const std::optional<std::chrono::system_clock::time_point>& updated_at) const
	{
		// A never-set temperature is null, not stale.
		if (!updated_at.has_value())
		{
			return false;
		}

		return (std::chrono::system_clock::now() - updated_at.value()) > TemperatureStalenessThreshold;
	}

	bool DataHub::AirTempIsStale() const
	{
		return IsStale(m_AirTempUpdatedAt);
	}

	bool DataHub::PoolTempIsStale() const
	{
		return IsStale(m_PoolTempUpdatedAt);
	}

	bool DataHub::SpaTempIsStale() const
	{
		return IsStale(m_SpaTempUpdatedAt);
	}

	std::optional<Kernel::Temperature> DataHub::CurrentTempForReporting(BodyOfWaterIds body_id) const
	{
		// An inactive body on a dual-body system keeps broadcasting a junk current temperature
		// (~1C) - surface it as unavailable rather than a misleading value.
		if (auto body = GetBody(body_id); body && !body->get().IsActive())
		{
			return std::nullopt;
		}

		switch (body_id)
		{
		case BodyOfWaterIds::Pool: return PoolTemp();
		case BodyOfWaterIds::Spa:  return SpaTemp();
		default:                   return std::nullopt;
		}
	}

	Kernel::ORP DataHub::ORP() const
	{
		return m_ORP;
	}

	Kernel::pH DataHub::pH() const
	{
		return m_pH;
	}

	ppm_quantity DataHub::SaltLevel() const
	{
		return m_SaltLevel;
	}

	void DataHub::ORP(const Kernel::ORP& orp)
	{
		// Chemistry is re-reported every poll; only fan out the event on an actual value change.
		const bool changed = !(m_ORP == orp);

		m_ORP = orp;

		if (!changed)
		{
			return;
		}

		EmitChemistryEvent([this](DataHub_ConfigEvent_Chemistry& update_event)
			{
				update_event.ORP(m_ORP);
			});
	}

	void DataHub::pH(const Kernel::pH& pH)
	{
		const bool changed = !(m_pH == pH);

		m_pH = pH;

		if (!changed)
		{
			return;
		}

		EmitChemistryEvent([this](DataHub_ConfigEvent_Chemistry& update_event)
			{
				update_event.pH(m_pH);
			});
	}

	void DataHub::SaltLevel(const ppm_quantity& salt_level_in_ppm)
	{
		const bool changed = (m_SaltLevel != salt_level_in_ppm);

		m_SaltLevel = salt_level_in_ppm;

		if (!changed)
		{
			return;
		}

		EmitChemistryEvent([this](DataHub_ConfigEvent_Chemistry& update_event)
			{
				update_event.SaltLevel(m_SaltLevel);
			});
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const
	{
		return Devices.FindByTrait(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, type);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::Auxillaries() const
	{
		return DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes::Auxillary);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::Chlorinators() const
	{
		return DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes::Chlorinator);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::Heaters() const
	{
		return DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes::Heater);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::Lights() const
	{
		return DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes::Light);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::Pumps() const
	{
		return DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes::Pump);
	}

	std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::FilterPumps() const
	{
		return Devices.FindByTrait(AuxillaryTraitsTypes::PumpTypeTrait{}, PumpTypes::FilterCirculation);
	}

	uint32_t DataHub::CountOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const
	{
		return Devices.CountByTrait(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, type);
	}

	bool DataHub::HasAnyOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const
	{
		return Devices.HasAnyByTrait(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, type);
	}

	uint32_t DataHub::CountFilterPumps() const
	{
		return Devices.CountByTrait(AuxillaryTraitsTypes::PumpTypeTrait{}, PumpTypes::FilterCirculation);
	}

	bool DataHub::HasAnyFilterPumps() const
	{
		return Devices.HasAnyByTrait(AuxillaryTraitsTypes::PumpTypeTrait{}, PumpTypes::FilterCirculation);
	}

	std::optional<std::shared_ptr<Kernel::AuxillaryDevice>> DataHub::FilterPump()
	{
		auto pumps = FilterPumps();
		if (pumps.empty())
		{
			return std::nullopt;
		}
		return pumps.front();
	}

	std::size_t DataHub::AdoptUntypedFilterPumps()
	{
		using namespace AuxillaryTraitsTypes;

		const auto equals_ignoring_case = [](const std::string& lhs, const std::string& rhs)
		{
			return std::ranges::equal(lhs, rhs, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
		};

		std::size_t adopted{ 0 };

		for (const auto& pump : Pumps())
		{
			if (pump->AuxillaryTraits.Has(PumpTypeTrait{}))
			{
				continue;
			}

			if (auto label = pump->AuxillaryTraits.TryGet(LabelTrait{}); label.has_value() && equals_ignoring_case(label.value(), LabelTrait::COMMON_LABEL_FILTER_PUMP))
			{
				pump->AuxillaryTraits.Set(PumpTypeTrait{}, PumpTypes::FilterCirculation);
				++adopted;

				LogInfo(Channel::Equipment, "DataHub: adopted an untyped 'Filter Pump' device as the filter circulation pump");
			}
		}

		return adopted;
	}

	std::optional<bool> DataHub::AnyFilterPumpRunning() const
	{
		const auto pumps = FilterPumps();
		if (pumps.empty())
		{
			// No filter pump discovered: report "unknown" rather than "not running", so a caller
			// never explains an idle chlorinator with a pump it has never seen.
			return std::nullopt;
		}

		for (const auto& pump : pumps)
		{
			if (auto status = pump->AuxillaryTraits.TryGet(AuxillaryTraitsTypes::PumpStatusTrait{});
				status.has_value() && (PumpStatuses::Running == status.value()))
			{
				return true;
			}
		}

		return false;
	}

	std::optional<uint8_t> DataHub::ChlorinatorResolvedSetpoint(const AuxillaryDevice& chlorinator) const
	{
		using namespace AuxillaryTraitsTypes;

		// Configured value first; the passive last-generating % is only a display fallback for a
		// chlorinator that has never had its setpoint scraped.
		if (const auto configured = ChlorinatorConfiguredSetpoint(chlorinator); configured.has_value())
		{
			return configured;
		}

		if (auto value = chlorinator.AuxillaryTraits.TryGet(ChlorinatorLastGeneratingTrait{}); value.has_value())
		{
			return static_cast<uint8_t>(value.value());
		}

		return std::nullopt;
	}

	std::optional<uint8_t> DataHub::ChlorinatorConfiguredSetpoint(const AuxillaryDevice& chlorinator) const
	{
		using namespace AuxillaryTraitsTypes;

		const auto trait_value = [&chlorinator](auto trait) -> std::optional<uint8_t>
		{
			if (auto value = chlorinator.AuxillaryTraits.TryGet(trait); value.has_value())
			{
				return static_cast<uint8_t>(value.value());
			}
			return std::nullopt;
		};

		const auto pool_setpoint = trait_value(ChlorinatorPoolSetpointTrait{});
		const auto spa_setpoint = trait_value(ChlorinatorSpaSetpointTrait{});

		bool spa_active = false;
		for (const auto& body : Bodies())
		{
			if (body.IsActive() && (BodyOfWaterIds::Spa == body.Id()))
			{
				spa_active = true;
				break;
			}
		}

		if (spa_active && spa_setpoint.has_value()) { return spa_setpoint; }
		if (!spa_active && pool_setpoint.has_value()) { return pool_setpoint; }
		if (pool_setpoint.has_value()) { return pool_setpoint; }   // active body unknown -> prefer pool
		if (spa_setpoint.has_value()) { return spa_setpoint; }

		return std::nullopt;
	}

	void DataHub::RequestChlorinatorSetpointRefresh()
	{
		m_ChlorinatorSetpointRefreshRequested = true;
	}

	bool DataHub::ConsumeChlorinatorSetpointRefreshRequest()
	{
		return std::exchange(m_ChlorinatorSetpointRefreshRequested, false);
	}

	void DataHub::NoteChlorinatorSetpointCommand()
	{
		m_LastChlorinatorSetpointCommand = std::chrono::steady_clock::now();
	}

	bool DataHub::ChlorinatorSetpointCommandWithin(std::chrono::seconds window) const
	{
		return m_LastChlorinatorSetpointCommand.has_value()
			&& ((std::chrono::steady_clock::now() - m_LastChlorinatorSetpointCommand.value()) < window);
	}

	void DataHub::MarkChlorinatorSetpointCached(BodyOfWaterIds body)
	{
		if (BodyOfWaterIds::Pool == body)
		{
			m_ChlorinatorPoolSetpointCached = true;
		}
		else if (BodyOfWaterIds::Spa == body)
		{
			m_ChlorinatorSpaSetpointCached = true;
		}
	}

	void DataHub::ConfirmChlorinatorSetpoint(BodyOfWaterIds body)
	{
		if (BodyOfWaterIds::Pool == body)
		{
			m_ChlorinatorPoolSetpointCached = false;
		}
		else if (BodyOfWaterIds::Spa == body)
		{
			m_ChlorinatorSpaSetpointCached = false;
		}
	}

	bool DataHub::ChlorinatorSetpointsCached() const
	{
		return m_ChlorinatorPoolSetpointCached || m_ChlorinatorSpaSetpointCached;
	}

	ChlorinatorGeneratingReason DataHub::ResolveChlorinatorGeneratingReason(const AuxillaryDevice& chlorinator) const
	{
		using namespace AuxillaryTraitsTypes;

		ChlorinatorGeneratingContext context;

		if (auto generating = chlorinator.AuxillaryTraits.TryGet(GeneratingPercentageTrait{}); generating.has_value())
		{
			context.GeneratingPercent = static_cast<uint8_t>(generating.value());
		}

		if (auto health = chlorinator.AuxillaryTraits.TryGet(ChlorinatorHealthTrait{}); health.has_value())
		{
			context.Health = health.value();
		}

		context.SetpointPercent = ChlorinatorResolvedSetpoint(chlorinator);
		context.FilterPumpRunning = AnyFilterPumpRunning();

		return ResolveGeneratingReason(context);
	}

	nlohmann::json DataHub::ChlorinatorSnapshot() const
	{
		using namespace AuxillaryTraitsTypes;

		const auto chlorinators = Chlorinators();
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
			: nlohmann::json(std::string{ magic_enum::enum_name(ChlorinatorStatuses::Unknown) });

		chlorinator["health"] = device->AuxillaryTraits.Has(ChlorinatorHealthTrait{})
			? nlohmann::json(std::string{ magic_enum::enum_name(*(device->AuxillaryTraits[ChlorinatorHealthTrait{}])) })
			: nlohmann::json(std::string{ magic_enum::enum_name(ChlorinatorHealth::Unknown) });

		// Every health flag currently active simultaneously (the wire status byte is a true
		// bitfield, so e.g. low salt and high current can both be set at once). "health" above
		// remains the single worst-of-the-set value for backward compatibility; this carries the
		// full set. Falls back to ["Unknown"] to mirror "health"'s own Unknown fallback.
		{
			nlohmann::json health_flags = nlohmann::json::array();
			if (device->AuxillaryTraits.Has(ChlorinatorHealthFlagsTrait{}))
			{
				for (const auto flag : *(device->AuxillaryTraits[ChlorinatorHealthFlagsTrait{}]))
				{
					health_flags.push_back(std::string{ magic_enum::enum_name(flag) });
				}
			}
			else
			{
				health_flags.push_back(std::string{ magic_enum::enum_name(ChlorinatorHealth::Unknown) });
			}
			chlorinator["health_flags"] = std::move(health_flags);
		}

		// Configured Pool / Spa output setpoints (scraped from the Set AquaPure menu, or written
		// through by a command), distinct from generating_percent (the instantaneous output).
		const auto pool_setpoint = device->AuxillaryTraits.Has(ChlorinatorPoolSetpointTrait{})
			? std::optional<uint8_t>(static_cast<uint8_t>(*(device->AuxillaryTraits[ChlorinatorPoolSetpointTrait{}])))
			: std::nullopt;
		const auto spa_setpoint = device->AuxillaryTraits.Has(ChlorinatorSpaSetpointTrait{})
			? std::optional<uint8_t>(static_cast<uint8_t>(*(device->AuxillaryTraits[ChlorinatorSpaSetpointTrait{}])))
			: std::nullopt;

		chlorinator["pool_setpoint_percent"] = pool_setpoint.has_value() ? nlohmann::json(pool_setpoint.value()) : nlohmann::json{};
		chlorinator["spa_setpoint_percent"] = spa_setpoint.has_value() ? nlohmann::json(spa_setpoint.value()) : nlohmann::json{};

		// True while a shown setpoint is still the value restored from the equipment cache (what the
		// panel held when the app last ran) rather than one read from the panel or just commanded.
		chlorinator["setpoints_cached"] =
			(pool_setpoint.has_value() && m_ChlorinatorPoolSetpointCached) ||
			(spa_setpoint.has_value() && m_ChlorinatorSpaSetpointCached);

		// Headline target = the configured setpoint of whichever body is currently active,
		// falling back to the other body's value, then to the passive last-known generating %.
		const auto resolved_setpoint = ChlorinatorResolvedSetpoint(*device);
		chlorinator["setpoint_percent"] = resolved_setpoint.has_value() ? nlohmann::json(resolved_setpoint.value()) : nlohmann::json{};

		// WHY the cell is (or is not) producing; derived once here so the web UI, MQTT and Home
		// Assistant all tell the same story.
		chlorinator["generating_reason"] = std::string{ magic_enum::enum_name(ResolveChlorinatorGeneratingReason(*device)) };

		return chlorinator;
	}

	bool DataHub::PublishChlorinatorStateIfChanged()
	{
		auto snapshot = ChlorinatorSnapshot();

		if (m_LastPublishedChlorinator.has_value() && (*m_LastPublishedChlorinator == snapshot))
		{
			return false;
		}

		// First observation with no chlorinator discovered: there is nothing to tell clients
		// (they already render the absence), so record the baseline without emitting.
		const bool first_and_absent = !m_LastPublishedChlorinator.has_value() && snapshot.is_null();
		m_LastPublishedChlorinator = snapshot;

		if (first_and_absent)
		{
			return false;
		}

		EmitChemistryEvent([&snapshot](DataHub_ConfigEvent_Chemistry& update_event)
			{
				update_event.Chlorinator(snapshot);
			});

		return true;
	}

	void DataHub::SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, std::string_view function)
	{
		m_SpaSwitchAssignments[std::make_pair(switch_number, button_number)] = function;
	}

	std::optional<std::string> DataHub::SpaSwitchAssignment(uint8_t switch_number, uint8_t button_number) const
	{
		const auto it = m_SpaSwitchAssignments.find(std::make_pair(switch_number, button_number));
		if (it == m_SpaSwitchAssignments.end())
		{
			return std::nullopt;
		}
		return it->second;
	}

	const std::map<std::pair<uint8_t, uint8_t>, std::string>& DataHub::SpaSwitchAssignments() const
	{
		return m_SpaSwitchAssignments;
	}

}
// namespace AqualinkAutomate::Kernel
