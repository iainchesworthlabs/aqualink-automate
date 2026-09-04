#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>

#include "http/server/http_server.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"

using namespace AqualinkAutomate;

//=============================================================================
// HttpServer / HttpSessionState exercised END-TO-END over the loopback
// interface: the acceptor, the per-IP connection cap, the request/response
// state machine, and the WebSocket-upgrade gate.  Everything runs on one
// io_context that the test pumps cooperatively (exactly how the kernel frame
// loop drives it), so there are no sleeps and no background threads.
//=============================================================================

inline constexpr char HSB_PING_ROUTE_URL[] = "/hsb/ping";
inline constexpr char HSB_WS_ROUTE_URL[] = "/hsb/ws";

namespace
{

	class HsbPingRoute final : public Interfaces::IWebRoute<HSB_PING_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			HTTP::Response res{ boost::beast::http::status::ok, req.version() };
			res.keep_alive(req.keep_alive());
			res.body() = "pong";
			res.prepare_payload();
			return res;
		}
	};

	class HsbWebSocket final : public Interfaces::IWebSocket<HSB_WS_ROUTE_URL>
	{
	public:
		std::optional<std::string> DequeueMessage(ConnectionId) override { return std::nullopt; }
		ConnectionId OnOpen() override { return 1; }
		void OnMessage(ConnectionId, const boost::beast::flat_buffer&) override {}
		void OnPublish(ConnectionId) override {}
		void OnClose(ConnectionId) override {}
		void OnError(ConnectionId) override {}
	};

	// Grab (and immediately release) an ephemeral port so the server under test
	// can bind a known one.  Racy only against another process claiming the very
	// same port in the microseconds in between, which no CI host does.
	unsigned short ReserveEphemeralPort(boost::asio::io_context& ioc)
	{
		boost::asio::ip::tcp::acceptor probe(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
		const unsigned short port = probe.local_endpoint().port();

		boost::system::error_code ec;
		probe.close(ec);

		return port;
	}

	// Cooperative pump: run at most one handler at a time (so control returns as
	// soon as something happened) until `pred` is satisfied or the budget runs
	// out.  Never blocks indefinitely, so a broken expectation fails the test
	// rather than hanging it.
	template<typename PRED>
	bool PumpUntil(boost::asio::io_context& ioc, HTTP::HttpServer& server, PRED pred, std::chrono::milliseconds budget = std::chrono::seconds(3))
	{
		const auto deadline = std::chrono::steady_clock::now() + budget;

		while (!pred() && (std::chrono::steady_clock::now() < deadline))
		{
			ioc.run_one_for(std::chrono::milliseconds(5));
			server.Poll();
		}

		return pred();
	}

	struct ServerFixture
	{
		ServerFixture()
		{
			HTTP::Routing::Clear();
			Port = ReserveEphemeralPort(Ioc);
			Endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), Port);
		}

		~ServerFixture()
		{
			HTTP::Routing::Clear();
		}

		boost::asio::ip::tcp::socket Connect()
		{
			boost::asio::ip::tcp::socket socket(Ioc);
			boost::system::error_code ec;
			socket.connect(Endpoint, ec);
			BOOST_REQUIRE_MESSAGE(!ec, "Failed to connect to the test server: " + ec.message());
			return socket;
		}

		boost::asio::io_context Ioc;
		unsigned short Port{ 0 };
		boost::asio::ip::tcp::endpoint Endpoint;
	};

	HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, bool keep_alive = true)
	{
		HTTP::Request req;
		req.version(11);
		req.method(method);
		req.target(target);
		req.set(boost::beast::http::field::host, "127.0.0.1");
		req.keep_alive(keep_alive);
		req.prepare_payload();
		return req;
	}

	// A raw (unencrypted) WebSocket upgrade request.  Written as a normal HTTP
	// request so the test can observe the server's REJECTION without needing a
	// full Beast websocket client handshake.
	HTTP::Request MakeUpgradeRequest(std::string_view target, std::string_view subprotocols)
	{
		HTTP::Request req;
		req.version(11);
		req.method(boost::beast::http::verb::get);
		req.target(target);
		req.set(boost::beast::http::field::host, "127.0.0.1");
		req.set(boost::beast::http::field::upgrade, "websocket");
		req.set(boost::beast::http::field::connection, "Upgrade");
		req.set(boost::beast::http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
		req.set(boost::beast::http::field::sec_websocket_version, "13");

		if (!subprotocols.empty())
		{
			req.set(boost::beast::http::field::sec_websocket_protocol, std::string{ subprotocols });
		}

		req.prepare_payload();
		return req;
	}

	// One request/response exchange over an already-connected socket.
	HTTP::Response Exchange(boost::asio::io_context& ioc, HTTP::HttpServer& server, boost::asio::ip::tcp::socket& socket, HTTP::Request& req)
	{
		bool written = false;
		boost::system::error_code write_ec;
		boost::beast::http::async_write(socket, req, [&written, &write_ec](boost::system::error_code ec, std::size_t)
			{
				write_ec = ec;
				written = true;
			});

		BOOST_REQUIRE_MESSAGE(PumpUntil(ioc, server, [&written] { return written; }), "Timed out writing the request");
		BOOST_REQUIRE_MESSAGE(!write_ec, "Failed to write the request: " + write_ec.message());

		HTTP::Response res;
		boost::beast::flat_buffer buffer;
		bool read = false;
		boost::system::error_code read_ec;
		boost::beast::http::async_read(socket, buffer, res, [&read, &read_ec](boost::system::error_code ec, std::size_t)
			{
				read_ec = ec;
				read = true;
			});

		BOOST_REQUIRE_MESSAGE(PumpUntil(ioc, server, [&read] { return read; }), "Timed out reading the response");
		BOOST_REQUIRE_MESSAGE(!read_ec, "Failed to read the response: " + read_ec.message());

		return res;
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_HttpServerBranches)

//=============================================================================
// Lifecycle
//=============================================================================

// Binding an address that does not belong to this host must fail cleanly
// (returning false), not throw and not leave the server "running".
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_Start_BindFailureReturnsFalse, ServerFixture)
{
	// 192.0.2.0/24 is TEST-NET-1 (RFC 5737): never assigned to a local interface.
	const boost::asio::ip::tcp::endpoint unroutable(boost::asio::ip::make_address("192.0.2.1"), Port);

	HTTP::HttpServer server(Ioc, unroutable);

	BOOST_CHECK(!server.Start());

	// A server that never started must tolerate Poll()/Stop() regardless.
	server.Poll();
	server.Stop();
}

// Poll()/Stop() before Start(), and Stop() twice, are all no-ops.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_PollAndStopAreSafeWithoutStart, ServerFixture)
{
	HTTP::HttpServer server(Ioc, Endpoint);

	server.Poll();
	server.Stop();
	server.Stop();

	// ...and Start() still works afterwards.
	BOOST_CHECK(server.Start());
	server.Poll();   // no sessions yet -> early-out
	server.Stop();
}

//=============================================================================
// HTTP request/response state machine
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_ServesRegisteredRoute, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	auto socket = Connect();

	auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/ping");
	auto res = Exchange(Ioc, server, socket, req);

	BOOST_CHECK(boost::beast::http::status::ok == res.result());
	BOOST_CHECK_EQUAL("pong", res.body());

	// Keep-alive: a second request on the SAME connection is served too.
	auto second = Exchange(Ioc, server, socket, req);
	BOOST_CHECK(boost::beast::http::status::ok == second.result());

	server.Stop();
}

BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_UnknownPathReturns404, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	auto socket = Connect();

	auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/nope");
	auto res = Exchange(Ioc, server, socket, req);

	BOOST_CHECK(boost::beast::http::status::not_found == res.result());

	server.Stop();
}

// A "Connection: close" exchange retires the session; the next Poll() sweeps it
// out of the server's session table.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_ConnectionCloseRetiresSession, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	auto socket = Connect();

	auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/ping", false);
	auto res = Exchange(Ioc, server, socket, req);

	BOOST_CHECK(boost::beast::http::status::ok == res.result());
	BOOST_CHECK(!res.keep_alive());

	// The server closes its end; the client observes end-of-stream.
	char scratch[8]{};
	bool completed = false;
	boost::system::error_code eof_ec;
	socket.async_read_some(boost::asio::buffer(scratch), [&completed, &eof_ec](boost::system::error_code ec, std::size_t)
		{
			eof_ec = ec;
			completed = true;
		});

	BOOST_CHECK(PumpUntil(Ioc, server, [&completed] { return completed; }));
	BOOST_CHECK(static_cast<bool>(eof_ec));

	server.Stop();
}

// A malformed request line must be answered (400) rather than crashing or
// hanging the session.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_MalformedRequestIsRejected, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	auto socket = Connect();

	// Not a valid HTTP request: Beast's parser rejects it and the session ends.
	static constexpr std::string_view GARBAGE{ "@@@ not-http @@@\r\n\r\n" };
	bool written = false;
	boost::asio::async_write(socket, boost::asio::buffer(GARBAGE.data(), GARBAGE.size()),
		[&written](boost::system::error_code, std::size_t) { written = true; });
	BOOST_REQUIRE(PumpUntil(Ioc, server, [&written] { return written; }));

	char scratch[64]{};
	bool completed = false;
	socket.async_read_some(boost::asio::buffer(scratch), [&completed](boost::system::error_code, std::size_t)
		{
			completed = true;
		});

	// Either an error response arrives or the peer closes; both are acceptable,
	// but the session must not simply stall.
	BOOST_CHECK(PumpUntil(Ioc, server, [&completed] { return completed; }));

	server.Stop();
}

//=============================================================================
// Per-IP connection cap
//=============================================================================

// A single peer must not be able to consume the whole connection budget: the
// server accepts up to MAX_CONNECTIONS_PER_IP (50) sessions from one address
// and immediately closes the next.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_PerIpConnectionCapClosesSurplus, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	constexpr std::size_t CAP = 50;

	std::vector<boost::asio::ip::tcp::socket> sockets;
	sockets.reserve(CAP + 1);

	for (std::size_t i = 0; i < CAP; ++i)
	{
		sockets.emplace_back(Connect());
	}

	// Let the acceptor take every one of them before the surplus arrives.
	PumpUntil(Ioc, server, [] { return false; }, std::chrono::milliseconds(200));

	sockets.emplace_back(Connect());

	char scratch[8]{};
	bool completed = false;
	boost::system::error_code surplus_ec;
	sockets.back().async_read_some(boost::asio::buffer(scratch), [&completed, &surplus_ec](boost::system::error_code ec, std::size_t)
		{
			surplus_ec = ec;
			completed = true;
		});

	BOOST_REQUIRE_MESSAGE(PumpUntil(Ioc, server, [&completed] { return completed; }), "The surplus connection was never closed");
	BOOST_CHECK_MESSAGE(static_cast<bool>(surplus_ec), "The surplus connection should have been closed by the server");

	// The connections inside the cap are unaffected and still serve requests.
	auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/ping");
	auto res = Exchange(Ioc, server, sockets.front(), req);
	BOOST_CHECK(boost::beast::http::status::ok == res.result());

	server.Stop();
}

//=============================================================================
// WebSocket upgrade gate
//=============================================================================

// An upgrade whose target has no registered socket is answered 404 instead of
// being handed to Beast's websocket accept.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_WsUpgradeUnknownTargetReturns404, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbWebSocket>());

	HTTP::HttpServer server(Ioc, Endpoint);
	BOOST_REQUIRE(server.Start());

	auto socket = Connect();

	auto req = MakeUpgradeRequest("/hsb/not-a-socket", "aqualink");
	auto res = Exchange(Ioc, server, socket, req);

	BOOST_CHECK(boost::beast::http::status::not_found == res.result());

	server.Stop();
}

// The upgrade is gated by the SAME security policy as an HTTP request: without
// the shared token the handshake is refused 401 before any websocket state is
// created.
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_WsUpgradeRejectedByPolicy, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbWebSocket>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "hsb-token";

	HTTP::HttpServer server(Ioc, Endpoint, std::nullopt, config);
	BOOST_REQUIRE(server.Start());

	{
		auto socket = Connect();

		// Offers the marker subprotocol but no credential.
		auto req = MakeUpgradeRequest("/hsb/ws", "aqualink");
		auto res = Exchange(Ioc, server, socket, req);

		BOOST_CHECK(boost::beast::http::status::unauthorized == res.result());
	}

	{
		auto socket = Connect();

		// A credential that does not match is refused just the same - and the
		// entry list is scanned past the marker to find it.
		auto req = MakeUpgradeRequest("/hsb/ws", "aqualink, bearer.wrong-token");
		auto res = Exchange(Ioc, server, socket, req);

		BOOST_CHECK(boost::beast::http::status::unauthorized == res.result());
	}

	server.Stop();
}

// An ordinary HTTP request is gated by the installed policy too: the server
// hands its SecurityConfig to the routing layer on Start().
BOOST_FIXTURE_TEST_CASE(Test_HttpServerBranches_SecurityConfigInstalledOnStart, ServerFixture)
{
	HTTP::Routing::Add(std::make_unique<HsbPingRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "hsb-token";

	HTTP::HttpServer server(Ioc, Endpoint, std::nullopt, config);
	BOOST_REQUIRE(server.Start());

	BOOST_CHECK(HTTP::Routing::GetSecurityConfig().AuthToken.has_value());

	{
		auto socket = Connect();
		auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/ping");
		BOOST_CHECK(boost::beast::http::status::unauthorized == Exchange(Ioc, server, socket, req).result());
	}

	{
		auto socket = Connect();
		auto req = MakeRequest(boost::beast::http::verb::get, "/hsb/ping");
		req.set(boost::beast::http::field::authorization, "Bearer hsb-token");
		auto res = Exchange(Ioc, server, socket, req);
		BOOST_CHECK(boost::beast::http::status::ok == res.result());
		BOOST_CHECK_EQUAL("pong", res.body());
	}

	server.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
