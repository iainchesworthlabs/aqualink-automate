#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "http/websocket_event_types.h"
#include "kernel/hub_events/data_hub_config_event.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "kernel/hub_events/data_hub_config_event_chemistry.h"
#include "kernel/hub_events/data_hub_config_event_circulation.h"
#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "kernel/hub_events/equipment_hub_system_event.h"
#include "kernel/hub_events/equipment_hub_system_event_status_change.h"

namespace AqualinkAutomate::HTTP
{

	class WebSocket_Event
	{
		static const std::string_view WS_JSON_TYPE_FIELD;
		static const std::string_view WS_JSON_PAYLOAD_FIELD;

	public:
		WebSocket_Event(const WebSocket_EventTypes& event_type, const nlohmann::json& payload);

		explicit WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& config_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_ButtonStateChange>& button_config_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Chemistry>& chem_config_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Circulation>& circ_config_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Temperature>& temp_config_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>& system_event);
		explicit WebSocket_Event(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent_StatusChange>& status_system_event);

		WebSocket_EventTypes Type() const;
		std::string Payload() const;

		std::string operator()() const;

	private:
		// Shared assignment path for every event-derived constructor: null-checks the
		// event, sets the type field, and moves the event's ToJSON() result into the
		// payload field. Templated so a single body covers all hub-event types.
		template<typename EVENT_TYPE>
		void SetFromEvent(WebSocket_EventTypes event_type, const std::shared_ptr<EVENT_TYPE>& event);

	private:
		WebSocket_EventTypes m_EventType;
		nlohmann::json m_EventPayload;

	public:
		static std::optional<WebSocket_Event> ConvertFromString(const std::string& event_payload);
		static std::optional<WebSocket_Event> ConvertFromStringView(const std::string_view& event_payload);
	};

}
// namespace AqualinkAutomate::HTTP
