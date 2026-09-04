#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include "alerting/alert_condition.h"
#include "alerting/webhook_sink.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Alerting;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

//=============================================================================
// WebhookSink — best-effort async POST of alert transitions.
//
// The sink runs entirely on the application io_context, so a tiny Boost.Beast
// HTTP server bound to 127.0.0.1:0 on the SAME io_context is enough to drive
// the whole connect -> write -> read -> shutdown path deterministically on one
// thread (mirrors the loopback MockMqttBroker pattern).  No network, no sleeps:
// the io_context is pumped until the expected observation lands.
//=============================================================================

namespace
{
	// Minimal loopback HTTP/1.1 server.  Records every request it receives and
	// answers with a configurable status.  Optionally drops the FIRST accepted
	// connection without answering (so the client's single retry is exercised).
	class LoopbackHttpServer
	{
	public:
		explicit LoopbackHttpServer(asio::io_context& io, http::status reply_status = http::status::ok, bool drop_first_connection = false) :
			m_Acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
			m_ReplyStatus(reply_status),
			m_DropFirstConnection(drop_first_connection)
		{
			Accept();
		}

		std::uint16_t Port() const
		{
			return m_Acceptor.local_endpoint().port();
		}

		std::string Url(std::string_view path_and_query) const
		{
			return std::format("http://127.0.0.1:{}{}", Port(), path_and_query);
		}

		std::string HttpsUrl(std::string_view path_and_query) const
		{
			return std::format("https://127.0.0.1:{}{}", Port(), path_and_query);
		}

		std::size_t Connections() const { return m_Connections; }
		const std::vector<http::request<http::string_body>>& Received() const { return m_Received; }

	private:
		struct Session
		{
			explicit Session(tcp::socket socket) : stream(std::move(socket)) {}

			beast::tcp_stream stream;
			beast::flat_buffer buffer;
			http::request<http::string_body> request;
			http::response<http::string_body> response;
		};

		void Accept()
		{
			m_Acceptor.async_accept([this](beast::error_code ec, tcp::socket socket)
			{
				if (ec)
				{
					return;
				}

				++m_Connections;

				if (m_DropFirstConnection && (1 == m_Connections))
				{
					// Drop the connection without a response: the client's read
					// fails with EOF and it must retry exactly once.
					beast::error_code ignored;
					socket.shutdown(tcp::socket::shutdown_both, ignored);
					socket.close(ignored);
				}
				else
				{
					auto session = std::make_shared<Session>(std::move(socket));
					http::async_read(session->stream, session->buffer, session->request, [this, session](beast::error_code read_ec, std::size_t)
					{
						if (read_ec)
						{
							return;
						}

						m_Received.push_back(session->request);

						session->response = http::response<http::string_body>{ m_ReplyStatus, session->request.version() };
						session->response.set(http::field::content_type, "text/plain");
						session->response.body() = "ok";
						session->response.prepare_payload();

						http::async_write(session->stream, session->response, [session](beast::error_code, std::size_t)
						{
							beast::error_code ignored;
							session->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
						});
					});
				}

				Accept();
			});
		}

	private:
		tcp::acceptor m_Acceptor;
		http::status m_ReplyStatus;
		bool m_DropFirstConnection;
		std::size_t m_Connections{ 0 };
		std::vector<http::request<http::string_body>> m_Received;
	};

	// Pump the io_context until `done` holds (or a generous deadline passes so a
	// broken test fails instead of hanging).  Returns whether `done` held.
	bool PumpUntil(asio::io_context& io, const std::function<bool()>& done, std::chrono::milliseconds deadline = std::chrono::seconds(5))
	{
		const auto end = std::chrono::steady_clock::now() + deadline;
		while (!done() && (std::chrono::steady_clock::now() < end))
		{
			io.run_one_for(std::chrono::milliseconds(20));
		}
		return done();
	}

	AlertTransition MakeTransition(bool raised, nlohmann::json params = nlohmann::json::object())
	{
		AlertTransition t;
		t.condition = std::string{ ConditionKeys::SaltLow };
		t.raised = raised;
		t.ts = 1'700'000'000;
		t.detail = raised ? "Salt level 2000 ppm is below 2600 ppm" : "Salt level recovered";
		t.params = std::move(params);
		return t;
	}
}
// anonymous namespace

BOOST_AUTO_TEST_SUITE(TestSuite_WebhookSink)

//-----------------------------------------------------------------------------
// BuildPayload (pure)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BuildPayload_Raised_CarriesConditionStateTsDetailAndParams)
{
	const auto payload = nlohmann::json::parse(WebhookSink::BuildPayload(MakeTransition(true, { { "salt_ppm", 2000.0 }, { "threshold_ppm", 2600.0 } })));

	BOOST_CHECK_EQUAL(payload.at("condition").get<std::string>(), std::string{ ConditionKeys::SaltLow });
	BOOST_CHECK_EQUAL(payload.at("state").get<std::string>(), "raised");
	BOOST_CHECK_EQUAL(payload.at("ts").get<std::int64_t>(), 1'700'000'000);
	BOOST_CHECK_EQUAL(payload.at("detail").get<std::string>(), "Salt level 2000 ppm is below 2600 ppm");
	BOOST_REQUIRE(payload.contains("params"));
	BOOST_CHECK_EQUAL(payload.at("params").at("salt_ppm").get<double>(), 2000.0);
	BOOST_CHECK_EQUAL(payload.at("params").at("threshold_ppm").get<double>(), 2600.0);
}

BOOST_AUTO_TEST_CASE(BuildPayload_Cleared_OmitsEmptyParams)
{
	const auto payload = nlohmann::json::parse(WebhookSink::BuildPayload(MakeTransition(false)));

	BOOST_CHECK_EQUAL(payload.at("state").get<std::string>(), "cleared");
	BOOST_CHECK(!payload.contains("params"));   // empty object => additive field omitted
}

BOOST_AUTO_TEST_CASE(BuildPayload_NonObjectParams_Omitted)
{
	// Only a non-empty OBJECT is forwarded; an array is not a params document.
	const auto payload = nlohmann::json::parse(WebhookSink::BuildPayload(MakeTransition(true, nlohmann::json::array({ 1, 2 }))));
	BOOST_CHECK(!payload.contains("params"));
}

//-----------------------------------------------------------------------------
// Post: no-op cases (nothing is spawned)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Post_NoUrlProvider_IsNoOp)
{
	asio::io_context io;
	WebhookSink sink(io, nullptr);

	BOOST_CHECK_NO_THROW(sink.Post(MakeTransition(true)));
	BOOST_CHECK_EQUAL(io.run(), 0u);   // nothing was co_spawned
}

BOOST_AUTO_TEST_CASE(Post_EmptyOrInvalidUrl_IsNoOp)
{
	const std::vector<std::string> urls{
		"",                        // unset preference
		"not a url at all",        // unparsable
		"ftp://example.test/x",    // unsupported scheme
		"http:///nohost",          // http without a host
		"mailto:someone@example.test",
	};

	for (const auto& url : urls)
	{
		asio::io_context io;
		WebhookSink sink(io, [url] { return url; });

		BOOST_CHECK_NO_THROW(sink.Post(MakeTransition(true)));
		BOOST_CHECK_MESSAGE(0u == io.run(), "no work should be spawned for URL '" << url << "'");
	}
}

//-----------------------------------------------------------------------------
// Post over plain HTTP to the loopback server
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Post_Http_DeliversJsonPostWithHostAndContentType)
{
	asio::io_context io;
	LoopbackHttpServer server(io);

	WebhookSink sink(io, [&server] { return server.Url("/hooks/alert?source=aqualink"); });

	sink.Post(MakeTransition(true, { { "salt_ppm", 2000.0 } }));

	BOOST_REQUIRE(PumpUntil(io, [&server] { return !server.Received().empty(); }));
	BOOST_REQUIRE_EQUAL(server.Received().size(), 1u);

	const auto& req = server.Received().front();
	BOOST_CHECK(http::verb::post == req.method());
	BOOST_CHECK_EQUAL(std::string{ req.target() }, "/hooks/alert?source=aqualink");   // path + query preserved
	BOOST_CHECK_EQUAL(std::string{ req[http::field::host] }, "127.0.0.1");
	BOOST_CHECK_EQUAL(std::string{ req[http::field::content_type] }, "application/json");
	BOOST_CHECK_EQUAL(std::string{ req[http::field::user_agent] }, "aqualink-automate");

	const auto body = nlohmann::json::parse(req.body());
	BOOST_CHECK_EQUAL(body.at("condition").get<std::string>(), std::string{ ConditionKeys::SaltLow });
	BOOST_CHECK_EQUAL(body.at("state").get<std::string>(), "raised");
	BOOST_CHECK_EQUAL(body.at("params").at("salt_ppm").get<double>(), 2000.0);

	// A 2xx answer is final: no retry, so exactly one connection was made.
	BOOST_CHECK_EQUAL(server.Connections(), 1u);
}

BOOST_AUTO_TEST_CASE(Post_Http_EmptyPath_DefaultsToRoot)
{
	asio::io_context io;
	LoopbackHttpServer server(io);

	// No path component at all -> the request target must be "/".
	WebhookSink sink(io, [&server] { return server.Url(""); });
	sink.Post(MakeTransition(false));

	BOOST_REQUIRE(PumpUntil(io, [&server] { return !server.Received().empty(); }));
	BOOST_CHECK_EQUAL(std::string{ server.Received().front().target() }, "/");
	BOOST_CHECK_EQUAL(nlohmann::json::parse(server.Received().front().body()).at("state").get<std::string>(), "cleared");
}

BOOST_AUTO_TEST_CASE(Post_Http_Non2xxResponse_IsAcceptedWithoutRetry)
{
	// The sink is fire-and-forget: a server-side 500 is not a transport failure
	// and must NOT trigger the retry (which is reserved for connect/IO errors).
	asio::io_context io;
	LoopbackHttpServer server(io, http::status::internal_server_error);

	WebhookSink sink(io, [&server] { return server.Url("/hook"); });
	sink.Post(MakeTransition(true));

	BOOST_REQUIRE(PumpUntil(io, [&server] { return !server.Received().empty(); }));

	// Give the sink a chance to (wrongly) reconnect; it must not.
	io.poll();
	BOOST_CHECK_EQUAL(server.Received().size(), 1u);
	BOOST_CHECK_EQUAL(server.Connections(), 1u);
}

BOOST_AUTO_TEST_CASE(Post_Http_DroppedFirstConnection_RetriesOnceAndSucceeds)
{
	asio::io_context io;
	LoopbackHttpServer server(io, http::status::ok, /*drop_first_connection=*/true);

	WebhookSink sink(io, [&server] { return server.Url("/hook"); });
	sink.Post(MakeTransition(true));

	// First attempt: connection dropped before any response (read fails).
	// Second attempt: served.  Exactly two connections, one delivered request.
	BOOST_REQUIRE(PumpUntil(io, [&server] { return !server.Received().empty(); }));
	BOOST_CHECK_EQUAL(server.Connections(), 2u);
	BOOST_CHECK_EQUAL(server.Received().size(), 1u);
}

BOOST_AUTO_TEST_CASE(Post_Http_UrlIsReadFreshOnEveryPost)
{
	asio::io_context io;
	LoopbackHttpServer first(io);
	LoopbackHttpServer second(io);

	std::string current = first.Url("/a");
	WebhookSink sink(io, [&current] { return current; });

	sink.Post(MakeTransition(true));
	BOOST_REQUIRE(PumpUntil(io, [&first] { return !first.Received().empty(); }));

	// Runtime preference change: the next transition goes to the new endpoint.
	current = second.Url("/b");
	sink.Post(MakeTransition(false));
	BOOST_REQUIRE(PumpUntil(io, [&second] { return !second.Received().empty(); }));

	BOOST_CHECK_EQUAL(std::string{ first.Received().front().target() }, "/a");
	BOOST_CHECK_EQUAL(std::string{ second.Received().front().target() }, "/b");
	BOOST_CHECK_EQUAL(first.Received().size(), 1u);
}

BOOST_AUTO_TEST_CASE(Post_Http_ConnectionRefused_RetriesThenDropsWithoutWedging)
{
	asio::io_context io;

	// Reserve a loopback port, then release it so nothing listens there.
	std::uint16_t dead_port = 0;
	{
		tcp::acceptor probe(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
		dead_port = probe.local_endpoint().port();
	}

	WebhookSink sink(io, [dead_port] { return std::format("http://127.0.0.1:{}/hook", dead_port); });
	BOOST_CHECK_NO_THROW(sink.Post(MakeTransition(true)));

	// Both attempts are refused immediately on loopback; the detached coroutine
	// must complete (logging + dropping) and leave the io_context with no work.
	BOOST_CHECK(PumpUntil(io, [&io] { return io.stopped(); }));
	BOOST_CHECK(io.stopped());
}

//-----------------------------------------------------------------------------
// HTTPS branch: the client TLS path up to the handshake, against a plain-TCP
// peer (no real TLS server needed).  The handshake fails on both attempts, so
// the request is dropped without affecting the io_context.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Post_Https_HandshakeFailure_RetriesThenDrops)
{
	asio::io_context io;
	LoopbackHttpServer server(io);   // plain TCP: cannot complete a TLS handshake

	WebhookSink sink(io, [&server] { return server.HttpsUrl("/secure"); });
	BOOST_CHECK_NO_THROW(sink.Post(MakeTransition(true)));

	// Two TCP connections (initial + retry) are made, but no HTTP request ever
	// arrives because the TLS handshake never completes against a plain peer.
	BOOST_REQUIRE(PumpUntil(io, [&server] { return server.Connections() >= 2; }));
	io.poll();
	BOOST_CHECK_EQUAL(server.Connections(), 2u);
	BOOST_CHECK(server.Received().empty());
}

BOOST_AUTO_TEST_SUITE_END()
