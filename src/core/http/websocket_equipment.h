#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <boost/signals2.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebsocket.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "kernel/hub_events/equipment_hub_system_event_status_change.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENT_WEBSOCKET_URL[] = "/ws/equipment";

	class WebSocket_Equipment : public Interfaces::IWebSocket<EQUIPMENT_WEBSOCKET_URL>
	{
	public:
		WebSocket_Equipment(Kernel::HubLocator& hub_locator);

		Interfaces::AccessRequirement RequiredAccess() const override
		{
			return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
		}

	public:
		std::optional<std::string> DequeueMessage(ConnectionId connId) override;

	public:
        ConnectionId OnOpen() override;
        void OnMessage(ConnectionId connId, const boost::beast::flat_buffer& buffer) override;
		void OnPublish(ConnectionId connId) override;
        void OnClose(ConnectionId connId) override;
        void OnError(ConnectionId connId) override;

	private:
		void Broadcast(const std::shared_ptr<const std::string>& payload);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub{ nullptr };

		// scoped_connection guarantees the hub-signal slots (which capture raw `this`)
		// are disconnected when this handler is destroyed, preventing a use-after-free
		// if a hub outlives the handler and fires a signal during/after teardown.
		boost::signals2::scoped_connection m_ConfigChangeSlot;
		boost::signals2::scoped_connection m_StatusChangeSlot;
		boost::signals2::scoped_connection m_AlertSlot;

		// NOTE: The connection/message-queue map is intentionally unsynchronised.
		// Signal-driven broadcasts, connection lifecycle, and message dequeuing
		// all run on the single application thread driven by the main poll() loop,
		// so no locking is required. If a multi-threaded execution model is ever
		// reintroduced, m_Connections must be guarded before concurrent access.
		//
		// Queued messages are held by shared_ptr<const std::string> so a single
		// broadcast shares one serialised payload across every connection queue
		// (refcount bump) rather than deep-copying the string N times.
		std::unordered_map<ConnectionId, std::deque<std::shared_ptr<const std::string>>> m_Connections;
		ConnectionId m_NextConnectionId{ 1 };

		// Count of broadcast messages dropped because a connection queue was full.
		// Surfaced via a rate-limited warning so silent state-update loss is visible.
		std::uint64_t m_DroppedMessageCount{ 0 };

		// Security: Maximum queue size per connection to prevent memory exhaustion
		static constexpr std::size_t MAX_MESSAGE_QUEUE_SIZE = 100;

		// Emit an overflow warning once every N drops to avoid log flooding.
		static constexpr std::uint64_t DROPPED_MESSAGE_LOG_INTERVAL = 100;
	};

}
// namespace AqualinkAutomate::HTTP
