#include <chrono>

#include <boost/test/unit_test.hpp>

#include "http/server/websocket_timeouts.h"

using namespace AqualinkAutomate;

//
// Regression guard for the WebSocket keepalive policy.
//
// The /ws/equipment channel is change-driven and can be silent for minutes
// when the pool is in a steady state.  Reverse proxies in front of the app
// commonly sever idle upstream connections (nginx defaults proxy_read_timeout
// to 60s; Cloudflare closes idle WebSockets at ~100s), which surfaced as a
// spurious "Connection lost — retrying..." toast followed by an immediate
// successful reconnect.  Beast sends a keepalive ping after idle_timeout/2 of
// silence, so the server policy must keep that ping interval inside the
// tightest common proxy window.  Reverting to
// stream_base::timeout::suggested(role_type::server) (300s idle, first ping
// at 150s) reintroduces the bug — these checks fail on such a revert.
//

BOOST_AUTO_TEST_SUITE(TestSuite_WebSocket_Keepalive)

BOOST_AUTO_TEST_CASE(Test_WebSocket_Keepalive_PingsEnabled)
{
	const auto opt = HTTP::WebSocketServerTimeout();

	BOOST_CHECK(opt.keep_alive_pings);
}

BOOST_AUTO_TEST_CASE(Test_WebSocket_Keepalive_PingIntervalBeatsCommonProxyIdleTimeouts)
{
	using namespace std::chrono_literals;

	const auto opt = HTTP::WebSocketServerTimeout();

	// The tightest idle timeout the policy must defeat: nginx's 60s
	// proxy_read_timeout default (Cloudflare's ~100s is looser).
	constexpr auto TIGHTEST_COMMON_PROXY_IDLE_TIMEOUT = 60s;

	// Beast pings after idle_timeout/2 of silence; that interval must sit
	// strictly inside the proxy window or an idle connection is severed
	// before the first ping goes out.
	const auto ping_interval = opt.idle_timeout / 2;

	BOOST_CHECK(ping_interval > std::chrono::seconds::zero());
	BOOST_CHECK(ping_interval < TIGHTEST_COMMON_PROXY_IDLE_TIMEOUT);
}

BOOST_AUTO_TEST_CASE(Test_WebSocket_Keepalive_DeadPeerStillDetected)
{
	using namespace std::chrono_literals;

	const auto opt = HTTP::WebSocketServerTimeout();

	// The idle timeout must remain finite (never stream_base::none()) so a
	// peer that stops answering pings is still torn down promptly.
	BOOST_CHECK(opt.idle_timeout > std::chrono::seconds::zero());
	BOOST_CHECK(opt.idle_timeout <= 300s);

	// The upgrade handshake keeps its own finite bound.
	BOOST_CHECK(opt.handshake_timeout > std::chrono::seconds::zero());
	BOOST_CHECK(opt.handshake_timeout <= 60s);
}

BOOST_AUTO_TEST_SUITE_END()
