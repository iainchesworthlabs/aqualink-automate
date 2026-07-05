#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "kernel/temperature.h"
#include "kernel/hub_events/data_hub_config_event.h"

namespace AqualinkAutomate::Kernel
{

	class DataHub_ConfigEvent_Temperature : public DataHub_ConfigEvent
	{
	public:
		DataHub_ConfigEvent_Temperature();
		~DataHub_ConfigEvent_Temperature() override = default;

	public:
		std::optional<Kernel::Temperature> PoolTemp() const;
		std::optional<Kernel::Temperature> SpaTemp() const;
		std::optional<Kernel::Temperature> AirTemp() const;
		std::optional<Kernel::Temperature> PoolSetpoint() const;
		std::optional<Kernel::Temperature> PoolSetpoint2() const;
		std::optional<bool> PoolHeater2Enabled() const;
		std::optional<Kernel::Temperature> SpaSetpoint() const;

	public:
		void PoolTemp(const Kernel::Temperature& pool);
		void SpaTemp(const Kernel::Temperature& spa);
		void AirTemp(const Kernel::Temperature& air);
		void PoolSetpoint(const Kernel::Temperature& pool_setpoint);
		void PoolSetpoint2(const Kernel::Temperature& pool_setpoint_2);
		void PoolHeater2Enabled(bool pool_heater_2_enabled);
		void SpaSetpoint(const Kernel::Temperature& spa_setpoint);

	public:
		nlohmann::json ToJSON() const override;

	private:
		std::optional<Kernel::Temperature> m_PoolTemp;
		std::optional<Kernel::Temperature> m_SpaTemp;
		std::optional<Kernel::Temperature> m_AirTemp;
		std::optional<Kernel::Temperature> m_PoolSetpoint;
		std::optional<Kernel::Temperature> m_PoolSetpoint2{ std::nullopt };
		std::optional<bool> m_PoolHeater2Enabled{ std::nullopt };
		std::optional<Kernel::Temperature> m_SpaSetpoint;
	};

}
// namespace AqualinkAutomate::Kernel
