#include <algorithm>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "http/websocket_event.h"
#include "logging/logging.h"
#include "utility/case_insensitive_comparision.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	const std::string_view WebSocket_Event::WS_JSON_TYPE_FIELD{ "type" };
	const std::string_view WebSocket_Event::WS_JSON_PAYLOAD_FIELD{ "payload" };

	WebSocket_Event::WebSocket_Event(const HTTP::WebSocket_EventTypes& event_type, const nlohmann::json& payload) :
		m_EventType(event_type)
	{
		m_EventPayload[WS_JSON_TYPE_FIELD] = magic_enum::enum_name(event_type);
		m_EventPayload[WS_JSON_PAYLOAD_FIELD] = payload;
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& config_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		if (nullptr == config_event)
		{
			LogDebug(Channel::Web, "Invalid event type; cannot process DataHub_ConfigEvent type and payload.");
		}
		else
		{
			switch (config_event->Type())
			{
			case Kernel::Hub_EventTypes::ButtonStateChange:
				SetFromEvent(WebSocket_EventTypes::ButtonStateChange, std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_ButtonStateChange>(config_event));
				break;

			case Kernel::Hub_EventTypes::Chemistry:
				SetFromEvent(WebSocket_EventTypes::ChemistryUpdate, std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_Chemistry>(config_event));
				break;

			case Kernel::Hub_EventTypes::Temperature:
				SetFromEvent(WebSocket_EventTypes::TemperatureUpdate, std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_Temperature>(config_event));
				break;

			case Kernel::Hub_EventTypes::Circulation:
				SetFromEvent(WebSocket_EventTypes::CirculationUpdate, std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_Circulation>(config_event));
				break;

			default:
				LogDebug(Channel::Web, "Unknown event type; cannot process DataHub_ConfigEvent type and payload.");
			}
		}
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_ButtonStateChange>& button_config_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		SetFromEvent(WebSocket_EventTypes::ButtonStateChange, button_config_event);
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Chemistry>& chem_config_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		SetFromEvent(WebSocket_EventTypes::ChemistryUpdate, chem_config_event);
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Circulation>& circ_config_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		SetFromEvent(WebSocket_EventTypes::CirculationUpdate, circ_config_event);
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::DataHub_ConfigEvent_Temperature>& temp_config_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		SetFromEvent(WebSocket_EventTypes::TemperatureUpdate, temp_config_event);
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>& system_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		if (nullptr == system_event)
		{
			LogDebug(Channel::Web, "Invalid event type; cannot process EquipmentHub_SystemEvent type and payload.");
		}
		else
		{
			switch (system_event->Type())
			{
			case Kernel::Hub_EventTypes::ServiceStatus:
				SetFromEvent(WebSocket_EventTypes::SystemStatusChange, std::dynamic_pointer_cast<Kernel::EquipmentHub_SystemEvent_StatusChange>(system_event));
				break;

			default:
				LogDebug(Channel::Web, "Unknown event type; cannot process EquipmentHub_SystemEvent type and payload.");
			}
		}
	}

	WebSocket_Event::WebSocket_Event(const std::shared_ptr<Kernel::EquipmentHub_SystemEvent_StatusChange>& status_system_event) :
		m_EventType(WebSocket_EventTypes::Unknown)
	{
		SetFromEvent(WebSocket_EventTypes::SystemStatusChange, status_system_event);
	}

	template<typename EVENT_TYPE>
	void WebSocket_Event::SetFromEvent(WebSocket_EventTypes event_type, const std::shared_ptr<EVENT_TYPE>& event)
	{
		if (nullptr == event)
		{
			LogDebug(Channel::Web, [event_type] { return std::format("Invalid {} event; cannot process type and payload.", magic_enum::enum_name(event_type)); });
		}
		else
		{
			m_EventType = event_type;
			m_EventPayload[WS_JSON_TYPE_FIELD] = magic_enum::enum_name(m_EventType);
			// ToJSON() returns by value; the temporary is move-assigned into the payload.
			m_EventPayload[WS_JSON_PAYLOAD_FIELD] = event->ToJSON();
		}
	}

	WebSocket_EventTypes WebSocket_Event::Type() const
	{
		return m_EventType;
	}

	std::string WebSocket_Event::Payload() const
	{
		return m_EventPayload.dump();
	}

	std::string WebSocket_Event::operator()() const
	{
		return Payload();
	}

	std::optional<WebSocket_Event> WebSocket_Event::ConvertFromString(const std::string& event_payload)
	{
		return ConvertFromStringView(event_payload);
	}

	std::optional<WebSocket_Event> WebSocket_Event::ConvertFromStringView(const std::string_view& event_payload)
	{
		std::optional<WebSocket_Event> return_event{std::nullopt};

		// Parse directly from the view's contiguous range; no intermediate std::string copy.
		// IMPORTANT: copy-initialise with '=', NOT 'auto x{...}'. Under gcc/libstdc++,
		// brace-initialising from a single nlohmann::json selects the initializer_list
		// constructor, which wraps the parsed object in a 1-element ARRAY -- then
		// contains("type") is always false and every event is silently rejected. (MSVC
		// treats 'auto x{e}' as copy-init, which is why this passed on Windows only.)
		auto parsed_event = nlohmann::json::parse(event_payload.begin(), event_payload.end(), nullptr, false, false);

		if (!parsed_event.contains(WS_JSON_TYPE_FIELD))
		{
			LogDebug(Channel::Web, "Invalid websocket event; does not contain type field");
		}
		else if (!parsed_event.contains(WS_JSON_PAYLOAD_FIELD))
		{
			LogDebug(Channel::Web, "Invalid websocket event; does not contain payload field");
		}
		else if (!parsed_event[WS_JSON_TYPE_FIELD].is_string())
		{
			LogDebug(Channel::Web, "Invalid websocket event; type field is invalid type");
		}
		else
		{
			const std::string ws_event_type_as_string = parsed_event[WS_JSON_TYPE_FIELD];

			auto ws_event_type = magic_enum::enum_cast<WebSocket_EventTypes>(ws_event_type_as_string, Utility::case_insensitive_comparision).value_or(WebSocket_EventTypes::Unknown);
			auto ws_event_payload = parsed_event[WS_JSON_PAYLOAD_FIELD];

			return WebSocket_Event(ws_event_type, ws_event_payload);
		}

		return return_event;
	}
}
// namespace AqualinkAutomate::HTTP
