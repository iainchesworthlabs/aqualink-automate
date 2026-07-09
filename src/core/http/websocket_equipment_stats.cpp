#include <cstdint>
#include <format>

#include "http/json/json_equipment.h"
#include "http/websocket_equipment_stats.h"
#include "http/websocket_event.h"
#include "http/websocket_event_types.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	WebSocket_Equipment_Stats::WebSocket_Equipment_Stats(Kernel::HubLocator& hub_locator) :
		Interfaces::IWebSocket<EQUIPMENTSTATS_WEBSOCKET_URL>(),
		m_StatisticsHub(hub_locator.Find<Kernel::StatisticsHub>())
	{
		if (m_StatisticsHub)
		{
			m_StatsSlot = m_StatisticsHub->MessageCounts.Signal().connect(
				[this](uint64_t)
				{
					for (auto& [id, conn] : m_Connections)
					{
						conn.dirty = true;
					}
				});
		}
	}

	std::optional<std::string> WebSocket_Equipment_Stats::DequeueMessage(ConnectionId connId)
	{
		auto it = m_Connections.find(connId);
		if (it == m_Connections.end())
		{
			return std::nullopt;
		}

		if (!it->second.queue.empty())
		{
			auto msg = std::move(it->second.queue.front());
			it->second.queue.pop_front();
			return msg;
		}

		if (it->second.dirty)
		{
			it->second.dirty = false;
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebSocket_Equipment_Stats::DequeueMessage", std::source_location::current());
			auto payload = HTTP::WebSocket_Event(HTTP::WebSocket_EventTypes::StatisticsUpdate, JSON::GenerateJson_Equipment_Stats(m_StatisticsHub)).Payload();
			zone->Value(payload.size());
			return payload;
		}

		return std::nullopt;
	}

	WebSocket_Equipment_Stats::ConnectionId WebSocket_Equipment_Stats::OnOpen()
	{
		auto connId = m_NextConnectionId++;
		m_Connections[connId] = {};
		LogDebug(Channel::Web, std::format("WebSocket_Equipment_Stats: connection {} opened ({} total)", connId, m_Connections.size()));
		return connId;
	}

	void WebSocket_Equipment_Stats::OnMessage(ConnectionId /*connId*/, const boost::beast::flat_buffer& /*buffer*/)
	{
		// Intentionally empty: this is a broadcast-only endpoint; inbound client
		// messages are ignored.
	}

	void WebSocket_Equipment_Stats::OnPublish(ConnectionId /*connId*/)
	{
		// Intentionally empty: publishing is signal-driven, so there is no
		// per-connection publish work to perform here.
	}

	void WebSocket_Equipment_Stats::OnClose(ConnectionId connId)
	{
		m_Connections.erase(connId);
		LogDebug(Channel::Web, std::format("WebSocket_Equipment_Stats: connection {} closed ({} remaining)", connId, m_Connections.size()));
	}

	void WebSocket_Equipment_Stats::OnError(ConnectionId connId)
	{
		m_Connections.erase(connId);
		LogDebug(Channel::Web, std::format("WebSocket_Equipment_Stats: connection {} error ({} remaining)", connId, m_Connections.size()));
	}

}
// namespace AqualinkAutomate::HTTP
