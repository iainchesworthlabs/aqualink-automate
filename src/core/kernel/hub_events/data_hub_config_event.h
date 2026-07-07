#pragma once

#include <boost/uuid/uuid.hpp>
#include <nlohmann/json.hpp>

#include "kernel/hub_events/hub_event.h"
#include "kernel/hub_events/hub_eventtypes.h"

namespace AqualinkAutomate::Kernel
{

	class DataHub_ConfigEvent : public Hub_Event
	{
	public:
		explicit DataHub_ConfigEvent(Hub_EventTypes event_type);
		~DataHub_ConfigEvent() override = default;
	};

}
// namespace AqualinkAutomate::Kernel
