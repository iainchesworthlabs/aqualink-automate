#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <boost/beast/core/flat_buffer.hpp>

#include "concepts/is_c_array.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Interfaces
{

    class IWebSocketBase
    {
    public:
        using ConnectionId = uint64_t;

        IWebSocketBase() = default;
        virtual ~IWebSocketBase() = default;

    public:
        virtual const std::string_view Route() const = 0;

    public:
        // The entitlement required to complete the upgrade handshake (evaluated
        // by AuthorizeWebSocketUpgrade when --auth-mode is enabled).  Live-data
        // sockets declare equipment.view; binary in v1 (D13) so there is no
        // per-subject payload filtering behind it.
        virtual AccessRequirement RequiredAccess() const { return {}; }

    public:
        virtual std::optional<std::string> DequeueMessage(ConnectionId connId) = 0;

    public:
        virtual ConnectionId OnOpen() = 0;
        virtual void OnMessage(ConnectionId connId, const boost::beast::flat_buffer& buffer) = 0;
        virtual void OnPublish(ConnectionId connId) = 0;
        virtual void OnClose(ConnectionId connId) = 0;
        virtual void OnError(ConnectionId connId) = 0;
    };

	template<const auto& ROUTE_URL>
	requires (Concepts::CArray<decltype(ROUTE_URL)>)
    class IWebSocket : public IWebSocketBase
	{
	public:
        IWebSocket() = default;
        virtual ~IWebSocket() = default;

	public:
        virtual const std::string_view Route() const final
        {
            return ROUTE_URL;
        }
	};
}
// namespace AqualinkAutomate::Interfaces
