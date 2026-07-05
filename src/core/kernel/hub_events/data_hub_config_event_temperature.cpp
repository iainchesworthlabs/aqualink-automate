#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "utility/json_serialization_helpers.h"

namespace AqualinkAutomate::Kernel
{

	DataHub_ConfigEvent_Temperature::DataHub_ConfigEvent_Temperature() :
		DataHub_ConfigEvent(Hub_EventTypes::Temperature),
		m_PoolTemp(std::nullopt),
		m_SpaTemp(std::nullopt),
		m_AirTemp(std::nullopt),
		m_PoolSetpoint(std::nullopt),
		m_SpaSetpoint(std::nullopt)
	{
	}


	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::PoolTemp() const
	{
		return m_PoolTemp;
	}

	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::SpaTemp() const
	{
		return m_SpaTemp;
	}

	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::AirTemp() const
	{
		return m_AirTemp;
	}

	void DataHub_ConfigEvent_Temperature::PoolTemp(const Kernel::Temperature& pool)
	{
		m_PoolTemp = pool;
	}

	void DataHub_ConfigEvent_Temperature::SpaTemp(const Kernel::Temperature& spa)
	{
		m_SpaTemp = spa;
	}

	void DataHub_ConfigEvent_Temperature::AirTemp(const Kernel::Temperature& air)
	{
		m_AirTemp = air;
	}

	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::PoolSetpoint() const
	{
		return m_PoolSetpoint;
	}

	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::SpaSetpoint() const
	{
		return m_SpaSetpoint;
	}

	void DataHub_ConfigEvent_Temperature::PoolSetpoint(const Kernel::Temperature& pool_setpoint)
	{
		m_PoolSetpoint = pool_setpoint;
	}

	std::optional<Kernel::Temperature> DataHub_ConfigEvent_Temperature::PoolSetpoint2() const
	{
		return m_PoolSetpoint2;
	}

	void DataHub_ConfigEvent_Temperature::PoolSetpoint2(const Kernel::Temperature& pool_setpoint_2)
	{
		m_PoolSetpoint2 = pool_setpoint_2;
	}

	std::optional<bool> DataHub_ConfigEvent_Temperature::PoolHeater2Enabled() const
	{
		return m_PoolHeater2Enabled;
	}

	void DataHub_ConfigEvent_Temperature::PoolHeater2Enabled(bool pool_heater_2_enabled)
	{
		m_PoolHeater2Enabled = pool_heater_2_enabled;
	}

	void DataHub_ConfigEvent_Temperature::SpaSetpoint(const Kernel::Temperature& spa_setpoint)
	{
		m_SpaSetpoint = spa_setpoint;
	}

	nlohmann::json DataHub_ConfigEvent_Temperature::ToJSON() const
	{
		nlohmann::json j;

		// Raw dual-unit objects ({celsius, fahrenheit}), matching the REST
		// /api/equipment shape and the setpoints below. Display formatting —
		// unit choice, locale digits — is the frontend's job (docs/i18n.md);
		// the old pre-formatted string path hardcoded Celsius and ignored the
		// Temperature_DisplayUnits preference.
		if (m_PoolTemp.has_value())
		{
			j["pool_temp"] = Utility::SerializeTemperature(m_PoolTemp.value());
		}

		if (m_SpaTemp.has_value())
		{
			j["spa_temp"] = Utility::SerializeTemperature(m_SpaTemp.value());
		}

		if (m_AirTemp.has_value())
		{
			j["air_temp"] = Utility::SerializeTemperature(m_AirTemp.value());
		}

		if (m_PoolSetpoint.has_value())
		{
			j["pool_setpoint"] = Utility::SerializeTemperature(m_PoolSetpoint.value());
		}

		if (m_PoolSetpoint2.has_value())
		{
			j["pool_setpoint_2"] = Utility::SerializeTemperature(m_PoolSetpoint2.value());
		}

		if (m_PoolHeater2Enabled.has_value())
		{
			j["pool_heater_2_enabled"] = m_PoolHeater2Enabled.value();
		}

		if (m_SpaSetpoint.has_value())
		{
			j["spa_setpoint"] = Utility::SerializeTemperature(m_SpaSetpoint.value());
		}

		return j;
	}

}
// namespace AqualinkAutomate::Kernel
