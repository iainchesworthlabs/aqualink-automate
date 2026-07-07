#include <cstdint>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "formatters/orp_formatter.h"
#include "formatters/ph_formatter.h"
#include "kernel/hub_events/data_hub_config_event_chemistry.h"
#include "utility/json_serialization_helpers.h"

namespace AqualinkAutomate::Kernel
{

	DataHub_ConfigEvent_Chemistry::DataHub_ConfigEvent_Chemistry() :
		DataHub_ConfigEvent(Hub_EventTypes::Chemistry)
	{
	}


	std::optional<Kernel::ORP> DataHub_ConfigEvent_Chemistry::ORP() const
	{
		return m_ORP;
	}

	std::optional<Kernel::pH> DataHub_ConfigEvent_Chemistry::pH() const
	{
		return m_pH;
	}

	std::optional<Units::ppm_quantity> DataHub_ConfigEvent_Chemistry::SaltLevel() const
	{
		return m_SaltLevel;
	}

	void DataHub_ConfigEvent_Chemistry::ORP(const Kernel::ORP& orp)
	{
		m_ORP = orp;
	}

	void DataHub_ConfigEvent_Chemistry::pH(const Kernel::pH& pH)
	{
		m_pH = pH;
	}

	void DataHub_ConfigEvent_Chemistry::SaltLevel(const Units::ppm_quantity& salt_level_in_ppm)
	{
		m_SaltLevel = salt_level_in_ppm;
	}

	nlohmann::json DataHub_ConfigEvent_Chemistry::ToJSON() const
	{
		nlohmann::json j;

		if (m_ORP.has_value())
		{
			auto orp = m_ORP.value();
			j["orp"] = static_cast<uint16_t>(orp().value());
		}

		if (m_pH.has_value())
		{
			auto ph = m_pH.value();
			// Re-round the float32 pH at the JSON boundary; a raw promotion to double reintroduces
			// noise (7.1 -> 7.099999904632568) that nlohmann would emit verbatim.
			j["ph"] = Utility::RoundToDecimalPlaces(static_cast<double>(ph()), 1);
		}

		if (m_SaltLevel.has_value())
		{
			auto salt_level = m_SaltLevel.value();
			j["salt_level"] = static_cast<uint16_t>(salt_level.value());
		}

		return j;
	}

}
// namespace AqualinkAutomate::Kernel
