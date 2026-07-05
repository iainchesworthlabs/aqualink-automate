#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <boost/signals2.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebsocket.h"
#include "kernel/hub_locator.h"
#include "kernel/statistics_hub.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENTSTATS_WEBSOCKET_URL[] = "/ws/equipment/stats";

	class WebSocket_Equipment_Stats: public Interfaces::IWebSocket<EQUIPMENTSTATS_WEBSOCKET_URL>
	{
	public:
		WebSocket_Equipment_Stats(Kernel::HubLocator& hub_locator);

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
		struct ConnectionState
		{
			std::deque<std::string> queue;
			bool dirty{ false };
		};

		std::shared_ptr<Kernel::StatisticsHub> m_StatisticsHub{ nullptr };

		// scoped_connection guarantees the stats-hub slot (which captures raw `this`)
		// is disconnected when this handler is destroyed, preventing a use-after-free.
		boost::signals2::scoped_connection m_StatsSlot;

		// NOTE: m_Connections is intentionally unsynchronised. The stats signal slot,
		// connection lifecycle (OnOpen/OnClose/OnError), and DequeueMessage all run on
		// the single application thread driven by the main poll() loop, so no locking
		// is required. If a multi-threaded execution model is ever reintroduced, this
		// map must be guarded before concurrent access. (Mirrors WebSocket_Equipment.)
		std::unordered_map<ConnectionId, ConnectionState> m_Connections;
		ConnectionId m_NextConnectionId{ 1 };
	};

}
// namespace AqualinkAutomate::HTTP
