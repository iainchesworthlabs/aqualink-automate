#pragma once

#include <nlohmann/json.hpp>

#include "interfaces/istatus.h"
#include "kernel/hub_events/equipment_hub_system_event.h"

namespace AqualinkAutomate::Kernel
{

	class EquipmentHub_SystemEvent_StatusChange : public EquipmentHub_SystemEvent
	{
	public:
		explicit EquipmentHub_SystemEvent_StatusChange(const Interfaces::IStatus& status);
		~EquipmentHub_SystemEvent_StatusChange() override = default;

		nlohmann::json ToJSON() const override;

	private:
		nlohmann::json m_JSON_Payload;
	};

}
// namespace AqualinkAutomate::Kernel
