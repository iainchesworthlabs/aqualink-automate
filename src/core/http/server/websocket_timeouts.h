#pragma once

#include <chrono>

#include <boost/beast/websocket/stream_base.hpp>

namespace AqualinkAutomate::HTTP
{

	// Keepalive/timeout policy for server-side WebSocket streams.
	//
	// Beast's suggested(server) profile (300s idle timeout, so the first
	// keepalive ping goes out only after 150s of silence) is too slow for the
	// change-driven /ws/equipment channel: with the pool in a steady state no
	// frames flow for minutes, and common reverse-proxy idle timeouts (nginx's
	// 60s proxy_read_timeout default, Cloudflare's ~100s) sever the quiet
	// connection before the first ping — the UI sees a spurious
	// "Connection lost" toast and immediately reconnects.
	//
	// Beast pings after idle_timeout/2 of silence, so a 60s idle timeout keeps
	// a ping on the wire every ~30s — inside every common proxy window — while
	// still detecting a dead peer within a minute.
	inline constexpr std::chrono::seconds WS_HANDSHAKE_TIMEOUT{ 30 };
	inline constexpr std::chrono::seconds WS_IDLE_TIMEOUT{ 60 };

	inline boost::beast::websocket::stream_base::timeout WebSocketServerTimeout()
	{
		boost::beast::websocket::stream_base::timeout opt{};

		opt.handshake_timeout = WS_HANDSHAKE_TIMEOUT;
		opt.idle_timeout = WS_IDLE_TIMEOUT;
		opt.keep_alive_pings = true;

		return opt;
	}

}
// namespace AqualinkAutomate::HTTP
