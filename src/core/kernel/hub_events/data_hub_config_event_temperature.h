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

		std::optional<Kernel::Temperature> PoolTemp() const;
		std::optional<Kernel::Temperature> SpaTemp() const;
		std::optional<Kernel::Temperature> AirTemp() const;
		std::optional<Kernel::Temperature> PoolSetpoint() const;
		std::optional<Kernel::Temperature> PoolSetpoint2() const;
		std::optional<bool> PoolHeater2Enabled() const;
		std::optional<Kernel::Temperature> SpaSetpoint() const;

		void PoolTemp(const Kernel::Temperature& pool);
		void SpaTemp(const Kernel::Temperature& spa);
		void AirTemp(const Kernel::Temperature& air);
		void PoolSetpoint(const Kernel::Temperature& pool_setpoint);
		void PoolSetpoint2(const Kernel::Temperature& pool_setpoint_2);
		void PoolHeater2Enabled(bool pool_heater_2_enabled);
		void SpaSetpoint(const Kernel::Temperature& spa_setpoint);

		nlohmann::json ToJSON() const override;

	private:
		std::optional<Kernel::Temperature> m_PoolTemp{ std::nullopt };
		std::optional<Kernel::Temperature> m_SpaTemp{ std::nullopt };
		std::optional<Kernel::Temperature> m_AirTemp{ std::nullopt };
		std::optional<Kernel::Temperature> m_PoolSetpoint{ std::nullopt };
		std::optional<Kernel::Temperature> m_PoolSetpoint2{ std::nullopt };
		std::optional<bool> m_PoolHeater2Enabled{ std::nullopt };
		std::optional<Kernel::Temperature> m_SpaSetpoint{ std::nullopt };
	};

}
// namespace AqualinkAutomate::Kernel
