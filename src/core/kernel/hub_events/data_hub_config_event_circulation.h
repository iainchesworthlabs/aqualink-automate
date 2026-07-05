#pragma once

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kernel/body_of_water_ids.h"
#include "kernel/circulation.h"
#include "kernel/hub_events/data_hub_config_event.h"

namespace AqualinkAutomate::Kernel
{

	// Fired when the decoded circulation state changes: the circulation mode and,
	// for a dual-body system, which body of water is currently active. Lets WS/MQTT
	// consumers track spa-mode without polling (the web UI drives its spa-mode toggle
	// from the per-body active state carried here).
	class DataHub_ConfigEvent_Circulation : public DataHub_ConfigEvent
	{
	public:
		DataHub_ConfigEvent_Circulation();
		~DataHub_ConfigEvent_Circulation() override = default;

		CirculationModes Mode() const;
		void Mode(CirculationModes mode);

		// Append a body's current active state (id + is_active) to the snapshot.
		void AddBody(BodyOfWaterIds id, bool is_active);

		nlohmann::json ToJSON() const override;

	private:
		CirculationModes m_Mode{ CirculationModes::Pool };
		std::vector<std::pair<BodyOfWaterIds, bool>> m_Bodies;
	};

}
// namespace AqualinkAutomate::Kernel
