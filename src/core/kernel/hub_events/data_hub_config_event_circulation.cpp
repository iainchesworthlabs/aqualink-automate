#include <string>

#include <magic_enum/magic_enum.hpp>

#include "kernel/hub_events/data_hub_config_event_circulation.h"

namespace AqualinkAutomate::Kernel
{

	DataHub_ConfigEvent_Circulation::DataHub_ConfigEvent_Circulation() :
		DataHub_ConfigEvent(Hub_EventTypes::Circulation)
	{
	}

	CirculationModes DataHub_ConfigEvent_Circulation::Mode() const
	{
		return m_Mode;
	}

	void DataHub_ConfigEvent_Circulation::Mode(CirculationModes mode)
	{
		m_Mode = mode;
	}

	void DataHub_ConfigEvent_Circulation::AddBody(BodyOfWaterIds id, bool is_active)
	{
		m_Bodies.emplace_back(id, is_active);
	}

	nlohmann::json DataHub_ConfigEvent_Circulation::ToJSON() const
	{
		const bool spa_mode = (CirculationModes::Spa == m_Mode
			|| CirculationModes::SpaFill == m_Mode
			|| CirculationModes::SpaDrain == m_Mode);

		nlohmann::json j;
		j["mode"] = std::string{ magic_enum::enum_name(m_Mode) };
		j["spa_mode"] = spa_mode;

		// active_body = the first body flagged active (null when none/unknown).
		j["active_body"] = nlohmann::json{};

		nlohmann::json bodies_array = nlohmann::json::array();
		for (const auto& [id, is_active] : m_Bodies)
		{
			const auto id_name = std::string{ magic_enum::enum_name(id) };
			bodies_array.push_back(nlohmann::json{ { "id", id_name }, { "is_active", is_active } });

			if (is_active && j["active_body"].is_null())
			{
				j["active_body"] = id_name;
			}
		}
		j["bodies"] = std::move(bodies_array);

		return j;
	}

}
// namespace AqualinkAutomate::Kernel
