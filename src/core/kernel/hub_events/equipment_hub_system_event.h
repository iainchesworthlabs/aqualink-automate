#pragma once

#include <boost/uuid/uuid.hpp>
#include <nlohmann/json.hpp>

#include "kernel/hub_events/hub_event.h"
#include "kernel/hub_events/hub_eventtypes.h"

namespace AqualinkAutomate::Kernel
{

	class EquipmentHub_SystemEvent : public Hub_Event
	{
	public:
		explicit EquipmentHub_SystemEvent(Hub_EventTypes event_type);
		~EquipmentHub_SystemEvent() override = default;
	};

}
// namespace AqualinkAutomate::Kernel
