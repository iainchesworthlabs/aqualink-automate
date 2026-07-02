#include <magic_enum/magic_enum.hpp>

#include "kernel/hub_events/equipment_hub_system_event_status_change.h"

namespace AqualinkAutomate::Kernel
{

	EquipmentHub_SystemEvent_StatusChange::EquipmentHub_SystemEvent_StatusChange(const Interfaces::IStatus& status) :
		EquipmentHub_SystemEvent(Hub_EventTypes::ServiceStatus),
		m_JSON_Payload(status)
	{
	}


	nlohmann::json EquipmentHub_SystemEvent_StatusChange::ToJSON() const
	{
		return m_JSON_Payload;
	}

}
// namespace AqualinkAutomate::Kernel
