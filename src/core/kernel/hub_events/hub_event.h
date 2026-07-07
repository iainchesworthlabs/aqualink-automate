#pragma once

#include <nlohmann/json.hpp>

#include "kernel/hub_events/hub_eventtypes.h"

namespace AqualinkAutomate::Kernel
{

	class Hub_Event
	{
	public:
		explicit Hub_Event(Hub_EventTypes event_type);
		virtual ~Hub_Event() = default;

		Hub_EventTypes Type() const;

		virtual nlohmann::json ToJSON() const = 0;

	private:
		Hub_EventTypes m_EventType;
	};

}
// namespace AqualinkAutomate::Kernel
