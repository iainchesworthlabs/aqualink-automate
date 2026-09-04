#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/url/parse_path.hpp>

#include "auth/entitlement.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/subject.h"
#include "http/server/routing/matches.h"
#include "http/server/routing/node.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::HTTP;

//=============================================================================
// Branch-level coverage for the routing layer:
//
//   * node.h  -- the radix-tree matcher's less-travelled arms: branching over
//                sibling children, the optional-segment consume / don't-consume
//                retry, the star segment's zero-segment fallback (and the
//                WriteCaptureAt bookmark write it performs), and nested
//                optional-only resource discovery.
//   * routing.cpp -- the dispatcher's error and policy arms: unparseable
//                targets, throwing (sync and async) handlers, the exactly-once
//                response guard, the deferred-route-through-the-sync-facade
//                fallback, unmatched-path 401-vs-404 selection, the failed-auth
//                rate limiter's success-clears and prune paths, trusted-proxy
//                X-Forwarded-For derivation (IPv6 + malformed CIDR), and the
//                WebSocket upgrade revalidator closure.
//
// Everything here drives the PUBLIC routing API only; no production seam is
// required.
//=============================================================================

inline constexpr char RB_OK_ROUTE_URL[] = "/rb/ping";
inline constexpr char RB_THROW_ROUTE_URL[] = "/rb/throws";
inline constexpr char RB_ASYNC_ROUTE_URL[] = "/rb/async";
inline constexpr char RB_ASYNC_THROW_ROUTE_URL[] = "/rb/async-throws";
inline constexpr char RB_ASYNC_NEVER_ROUTE_URL[] = "/rb/async-never";
inline constexpr char RB_WS_ROUTE_URL[] = "/rb/ws/feed";

namespace
{

	//-------------------------------------------------------------------------
	// Test routes
	//-------------------------------------------------------------------------

	HTTP::Response MakeOkResponse(const HTTP::Request& req, std::string_view body)
	{
		HTTP::Response res{ boost::beast::http::status::ok, req.version() };
		res.keep_alive(req.keep_alive());
		res.body() = std::string{ body };
		res.prepare_payload();
		return res;
	}

	class RbOkRoute final : public Interfaces::IWebRoute<RB_OK_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			return MakeOkResponse(req, "pong");
		}
	};

	// A handler that escapes with an exception -> the dispatcher's catch arm.
	class RbThrowingRoute final : public Interfaces::IWebRoute<RB_THROW_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request&) override
		{
			throw std::runtime_error("route blew up");
		}
	};

	// A deferred-response route that completes INLINE (the common async shape).
	class RbAsyncRoute final : public Interfaces::IWebRoute<RB_ASYNC_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			return MakeOkResponse(req, "async");
		}

		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override
		{
			complete(MakeOkResponse(req, "async"));
		}
	};

	// Responds, THEN throws: the dispatcher's exactly-once guard must swallow
	// the 500 the catch handler would otherwise write over the top.
	class RbAsyncThrowingRoute final : public Interfaces::IWebRoute<RB_ASYNC_THROW_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			return MakeOkResponse(req, "already-answered");
		}

		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override
		{
			complete(MakeOkResponse(req, "already-answered"));
			throw std::runtime_error("after responding");
		}
	};

	// Never completes: dispatched through the SYNCHRONOUS facade this must
	// degrade to a 500 rather than returning an empty message.
	class RbAsyncNeverRoute final : public Interfaces::IWebRoute<RB_ASYNC_NEVER_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			return MakeOkResponse(req, "never");
		}

		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request&, AsyncCompletion) override
		{
			// Deliberately drops the completion.
		}
	};

	class RbWebSocket final : public Interfaces::IWebSocket<RB_WS_ROUTE_URL>
	{
	public:
		Interfaces::AccessRequirement RequiredAccess() const override
		{
			return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
		}

		std::optional<std::string> DequeueMessage(ConnectionId) override { return std::nullopt; }
		ConnectionId OnOpen() override { return 1; }
		void OnMessage(ConnectionId, const boost::beast::flat_buffer&) override {}
		void OnPublish(ConnectionId) override {}
		void OnClose(ConnectionId) override {}
		void OnError(ConnectionId) override {}
	};

	//-------------------------------------------------------------------------
	// Request helpers (same shape as test_http_security.cpp / test_routing_authz.cpp)
	//-------------------------------------------------------------------------

	HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target)
	{
		HTTP::Request req;
		req.version(11);
		req.method(method);
		req.target(target);
		req.set(boost::beast::http::field::host, "localhost.localdomain");
		return req;
	}

	HTTP::Response RunRequest(HTTP::Request& req, std::string_view peer_ip = {})
	{
		auto msg = HTTP::Routing::HTTP_OnRequest(req, peer_ip);

		boost::asio::io_context ioc;
		auto exec = ioc.get_executor();

		Test::MockBeastBasicStreamWithTimeout client_stream(exec);
		Test::MockBeastBasicStreamWithTimeout server_stream(exec);
		server_stream.connect(client_stream);

		boost::beast::error_code ec;
		boost::beast::write(server_stream, std::move(msg), ec);
		BOOST_REQUIRE_MESSAGE(!ec, "Failed to write response: " + ec.message());
		server_stream.close();

		ioc.poll();

		HTTP::Response resp;
		boost::beast::flat_buffer read_buffer;
		boost::beast::http::read(client_stream, read_buffer, resp, ec);
		BOOST_REQUIRE_MESSAGE(!ec || ec == boost::beast::http::error::end_of_stream, "Failed to read response: " + ec.message());

		// NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
		return resp;
	}

	// A subject resolver that hands back a fixed, caller-shaped Subject.  Avoids
	// the JWT/user-store machinery: the routing layer only consumes the Subject.
	Auth::Subject MakeSubject(bool authenticated, const std::vector<std::string>& entitlements)
	{
		Auth::Subject subject;
		subject.Id = authenticated ? "rb-user" : "anonymous";
		subject.Authenticated = authenticated;
		subject.Provider = authenticated ? Auth::SubjectProvider::Local : Auth::SubjectProvider::Anonymous;
		subject.Entitlements = Auth::EntitlementSet::Parse(entitlements);
		return subject;
	}

	struct RoutingFixture
	{
		RoutingFixture()
		{
			HTTP::Routing::Clear();
		}

		~RoutingFixture()
		{
			HTTP::Routing::Clear();
		}
	};

	class TreeHandler
	{
	};

	// Resolve `target` against `tree`, optionally with the bounds-checked
	// capture cursors, and report both the handler and the first capture.
	TreeHandler* Resolve(const Routing::impl<TreeHandler>& tree, std::string_view target, Routing::matches& m, bool bounded = true)
	{
		std::string_view* matches_it = m.matches();
		std::string_view* ids_it = m.ids();
		std::string_view* matches_end = bounded ? (m.matches() + m.size()) : nullptr;
		std::string_view* ids_end = bounded ? (m.ids() + m.size()) : nullptr;

		auto path = boost::urls::parse_path(target);
		BOOST_TEST_REQUIRE(!path.has_error());
		return tree.find_impl(*path, matches_it, ids_it, matches_end, ids_end);
	}

	std::string Decoded(const Routing::matches& m, std::size_t index)
	{
		std::string out;
		boost::urls::pct_string_view(m[index]).decode({}, boost::urls::string_token::append_to(out));
		return out;
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_RoutingBranches)

//=============================================================================
// node.h -- matcher arms
//=============================================================================

// Sibling children force the matcher's "branch" mode: a literal child can no
// longer simply be consumed, it must be tried RECURSIVELY (and abandoned when
// the continuation fails).  Likewise a plain {var} child alongside a modifier
// child is captured-then-recursed, rewinding the capture on failure.
BOOST_AUTO_TEST_CASE(Test_RoutingBranches_Node_BranchingSiblings)
{
	auto literal = std::make_shared<TreeHandler>();
	auto var_deep = std::make_shared<TreeHandler>();
	auto star = std::make_shared<TreeHandler>();

	Routing::impl<TreeHandler> tree;
	tree.insert_impl("/br/lit/leaf", literal.get());
	tree.insert_impl("/br/{id}/deep", var_deep.get());
	tree.insert_impl("/br/{rest*}", star.get());

	{
		// Literal sibling wins - reached through the branching (recursive) arm.
		Routing::matches m;
		BOOST_CHECK_EQUAL(literal.get(), Resolve(tree, "/br/lit/leaf", m));
	}

	{
		// {id} matches, is captured, and its continuation "/deep" succeeds.
		Routing::matches m;
		BOOST_CHECK_EQUAL(var_deep.get(), Resolve(tree, "/br/alpha/deep", m));
		BOOST_CHECK_EQUAL("alpha", Decoded(m, 0));
	}

	{
		// {id} matches the first segment but its continuation fails, so the
		// capture is rewound and the star sibling takes the whole path.
		Routing::matches m;
		BOOST_CHECK_EQUAL(star.get(), Resolve(tree, "/br/alpha/beta/gamma", m));
		BOOST_CHECK_EQUAL("alpha/beta/gamma", Decoded(m, 0));
	}

	{
		// The literal prefix "lit" exists but "/br/lit" alone carries no
		// resource; the star sibling answers instead.
		Routing::matches m;
		BOOST_CHECK_EQUAL(star.get(), Resolve(tree, "/br/lit", m));
	}
}

// An optional segment is tried BOTH ways: first consuming the segment (the
// longest match), then - when that continuation dead-ends - re-tried consuming
// nothing so the following literal can match.
BOOST_AUTO_TEST_CASE(Test_RoutingBranches_Node_OptionalConsumeAndSkip)
{
	auto tail = std::make_shared<TreeHandler>();

	Routing::impl<TreeHandler> tree;
	tree.insert_impl("/opt/{maybe?}/tail", tail.get());

	{
		// Consumes the optional segment.
		Routing::matches m;
		BOOST_CHECK_EQUAL(tail.get(), Resolve(tree, "/opt/value/tail", m));
		BOOST_CHECK_EQUAL("value", Decoded(m, 0));
	}

	{
		// Consuming "tail" as the optional dead-ends, so the matcher rewinds
		// and re-runs with the optional matching nothing.
		Routing::matches m;
		BOOST_CHECK_EQUAL(tail.get(), Resolve(tree, "/opt/tail", m));
		BOOST_CHECK_EQUAL("", Decoded(m, 0));
	}

	{
		// Neither arrangement reaches a resource.
		Routing::matches m;
		BOOST_CHECK_EQUAL(nullptr, Resolve(tree, "/opt/one/two/tail", m));
	}
}

// A {*} segment that must match ZERO segments: every non-empty subrange fails,
// so the matcher falls through to the "consume nothing" attempt and blanks the
// bookmarked capture slot it had speculatively written.
BOOST_AUTO_TEST_CASE(Test_RoutingBranches_Node_StarMatchesNoSegments)
{
	auto handler = std::make_shared<TreeHandler>();

	Routing::impl<TreeHandler> tree;
	tree.insert_impl("/st/{path*}/tail", handler.get());

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/st/tail", m));
		BOOST_CHECK_EQUAL("", Decoded(m, 0));
	}

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/st/a/b/tail", m));
		BOOST_CHECK_EQUAL("a/b", Decoded(m, 0));
	}

	{
		// Same shape through the LEGACY (unbounded) cursors: the end pointers
		// are optional and their absence must not change the outcome.
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/st/tail", m, false));
		BOOST_CHECK_EQUAL("", Decoded(m, 0));
	}
}

// The consumed path stops on a node that carries no resource, but a chain of
// OPTIONAL children below it does - the matcher must walk down to it without
// consuming further input.
BOOST_AUTO_TEST_CASE(Test_RoutingBranches_Node_NestedOptionalResource)
{
	auto handler = std::make_shared<TreeHandler>();
	auto sibling = std::make_shared<TreeHandler>();

	Routing::impl<TreeHandler> tree;
	tree.insert_impl("/oo/{a?}/{b?}", handler.get());
	tree.insert_impl("/oo/literal/leaf", sibling.get());

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/oo", m));
	}

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(sibling.get(), Resolve(tree, "/oo/literal/leaf", m));
	}
}

// Dot segments in the REQUEST path are normalised while matching: "." is
// skipped and ".." walks back up the (implicit) tree.
BOOST_AUTO_TEST_CASE(Test_RoutingBranches_Node_DotSegmentsInRequestPath)
{
	auto handler = std::make_shared<TreeHandler>();

	Routing::impl<TreeHandler> tree;
	tree.insert_impl("/dot/target", handler.get());

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/dot/./target", m));
	}

	{
		Routing::matches m;
		BOOST_CHECK_EQUAL(handler.get(), Resolve(tree, "/dot/other/../target", m));
	}

	{
		// Walks above the root and never comes back down to a resource.
		Routing::matches m;
		BOOST_CHECK_EQUAL(nullptr, Resolve(tree, "/dot/../..", m));
	}
}

//=============================================================================
// routing.cpp -- dispatcher error arms
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_UnparseableTarget_Returns400, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	// Absolute-form is not an origin-form target, so parsing fails outright.
	auto req = MakeRequest(boost::beast::http::verb::get, "http://example.com/rb/ping");

	BOOST_CHECK(boost::beast::http::status::bad_request == RunRequest(req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_ThrowingRoute_Returns500, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbThrowingRoute>());

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/throws");

	BOOST_CHECK(boost::beast::http::status::internal_server_error == RunRequest(req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_AsyncRoute_CompletesInline, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbAsyncRoute>());

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/async");
	auto resp = RunRequest(req);

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	BOOST_CHECK_EQUAL("async", resp.body());
	// The dispatcher stamps no-store on the deferred path too.
	BOOST_CHECK_EQUAL("no-store", std::string(resp[boost::beast::http::field::cache_control]));
}

// The exactly-once guard: the handler answered before throwing, so the catch
// arm's 500 must be dropped rather than written over the real response.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_AsyncRouteRespondsThenThrows_KeepsFirstResponse, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbAsyncThrowingRoute>());

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/async-throws");
	auto resp = RunRequest(req);

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	BOOST_CHECK_EQUAL("already-answered", resp.body());
}

// A route that never completes cannot be served by the synchronous facade.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_DeferredRouteThroughSyncFacade_Returns500, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbAsyncNeverRoute>());

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/async-never");

	BOOST_CHECK(boost::beast::http::status::internal_server_error == RunRequest(req).result());
}

//=============================================================================
// routing.cpp -- unmatched-path 401 vs 404
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_UnmatchedPath_TokenRequired_Returns401, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "s3cret";
	HTTP::Routing::SetSecurityConfig(std::move(config));

	// An unknown path must not leak its non-existence to an unauthenticated caller.
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/does-not-exist");

	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(req, "10.0.0.1").result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_UnmatchedPath_AuthMode_AnonymousUnderApi_Returns401, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	HTTP::Routing::SetSecurityConfig(std::move(config));
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(false, {}); });

	auto api_req = MakeRequest(boost::beast::http::verb::get, "/api/does-not-exist");
	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(api_req).result());

	// Outside /api the route surface is not sensitive: a plain 404 is correct.
	auto other_req = MakeRequest(boost::beast::http::verb::get, "/not-an-api-path");
	BOOST_CHECK(boost::beast::http::status::not_found == RunRequest(other_req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_UnmatchedPath_AuthMode_Authenticated_Returns404, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	HTTP::Routing::SetSecurityConfig(std::move(config));
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(true, { "equipment.view" }); });

	// Authenticated: the honest answer is 404.
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/does-not-exist");
	BOOST_CHECK(boost::beast::http::status::not_found == RunRequest(req).result());
}

//=============================================================================
// routing.cpp -- failed-auth rate limiter
//=============================================================================

namespace
{

	HTTP::Request MakeBearerRequest(std::string_view target, std::string_view token)
	{
		auto req = MakeRequest(boost::beast::http::verb::get, target);
		if (!token.empty())
		{
			req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ token });
		}
		return req;
	}

}
// unnamed namespace

// A genuine success must CLEAR the accumulated failures for that source: with
// nine failures either side of it, an unclearing limiter would have banned the
// IP long before the final (correct-token) request.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_RateLimit_SuccessClearsFailures, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "correct-horse";
	HTTP::Routing::SetSecurityConfig(std::move(config));

	constexpr std::string_view PEER{ "203.0.113.7" };

	for (int i = 0; i < 9; ++i)
	{
		auto bad = MakeBearerRequest("/rb/ping", "wrong");
		BOOST_REQUIRE(boost::beast::http::status::unauthorized == RunRequest(bad, PEER).result());
	}

	auto good = MakeBearerRequest("/rb/ping", "correct-horse");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(good, PEER).result());

	for (int i = 0; i < 9; ++i)
	{
		auto bad = MakeBearerRequest("/rb/ping", "wrong");
		BOOST_REQUIRE(boost::beast::http::status::unauthorized == RunRequest(bad, PEER).result());
	}

	auto still_good = MakeBearerRequest("/rb/ping", "correct-horse");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(still_good, PEER).result());
}

// Ten failures ban the source; the ban is checked BEFORE the token, so even a
// correct token is refused with 429 while it holds.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_RateLimit_BanPrecedesTokenCheck, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "correct-horse";
	HTTP::Routing::SetSecurityConfig(std::move(config));

	constexpr std::string_view PEER{ "203.0.113.9" };

	for (int i = 0; i < 10; ++i)
	{
		auto bad = MakeBearerRequest("/rb/ping", "wrong");
		BOOST_REQUIRE(boost::beast::http::status::unauthorized == RunRequest(bad, PEER).result());
	}

	auto good = MakeBearerRequest("/rb/ping", "correct-horse");
	auto resp = RunRequest(good, PEER);
	BOOST_CHECK(boost::beast::http::status::too_many_requests == resp.result());
	BOOST_CHECK_EQUAL("60", std::string(resp[boost::beast::http::field::retry_after]));
}

// The failure table is bounded: once it grows past its cap the limiter prunes
// on the next recorded failure.  Tracking must survive the prune - the very
// source that tripped it is still answered 401, not 500 or 200.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_RateLimit_PrunesLargeTable, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "correct-horse";
	HTTP::Routing::SetSecurityConfig(std::move(config));

	// MAX_TRACKED_IPS is 8192; fill the table with distinct single-failure
	// sources (responses are discarded - only the side effect matters here).
	for (int a = 0; a < 32; ++a)
	{
		for (int b = 0; b < 256; ++b)
		{
			auto req = MakeBearerRequest("/rb/ping", "wrong");
			const std::string peer = "10." + std::to_string(a) + "." + std::to_string(b) + ".1";
			auto discarded = HTTP::Routing::HTTP_OnRequest(req, peer);
			(void)discarded;
		}
	}

	// The next distinct source trips the prune sweep.
	auto tripping = MakeBearerRequest("/rb/ping", "wrong");
	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(tripping, "198.51.100.4").result());

	// ...and a valid credential still gets through afterwards.
	auto good = MakeBearerRequest("/rb/ping", "correct-horse");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(good, "198.51.100.5").result());
}

//=============================================================================
// routing.cpp -- EffectiveClientIp / trusted proxies
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_NoTrustedProxies, RoutingFixture)
{
	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
	req.set("X-Forwarded-For", "1.2.3.4");

	// Nothing trusted -> XFF ignored entirely.
	BOOST_CHECK_EQUAL("192.0.2.10", HTTP::Routing::EffectiveClientIp(req, "192.0.2.10"));

	// An empty peer stays empty regardless.
	BOOST_CHECK_EQUAL("", HTTP::Routing::EffectiveClientIp(req, ""));
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_UnparseablePeer, RoutingFixture)
{
	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "10.0.0.0/8" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
	req.set("X-Forwarded-For", "1.2.3.4");

	// The peer address cannot be parsed -> no trust is extended.
	BOOST_CHECK_EQUAL("not-an-ip-address", HTTP::Routing::EffectiveClientIp(req, "not-an-ip-address"));
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_MalformedCidrFailsClosed, RoutingFixture)
{
	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "not-a-cidr", "10.0.0.0/not-a-length" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
	req.set("X-Forwarded-For", "1.2.3.4");

	// A CIDR that will not parse must never be treated as a match.
	BOOST_CHECK_EQUAL("10.1.2.3", HTTP::Routing::EffectiveClientIp(req, "10.1.2.3"));
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_MismatchedFamilyFailsClosed, RoutingFixture)
{
	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "::1/128" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
	req.set("X-Forwarded-For", "1.2.3.4");

	// An IPv4 peer against an IPv6-only allow-list: no trust.
	BOOST_CHECK_EQUAL("10.1.2.3", HTTP::Routing::EffectiveClientIp(req, "10.1.2.3"));
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_IPv6TrustedProxy, RoutingFixture)
{
	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "::1/128" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	{
		auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
		req.set("X-Forwarded-For", "2001:db8::5, ::1");
		BOOST_CHECK_EQUAL("2001:db8::5", HTTP::Routing::EffectiveClientIp(req, "::1"));
	}

	{
		// Trusted, but nothing forwarded -> the peer itself.
		auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
		BOOST_CHECK_EQUAL("::1", HTTP::Routing::EffectiveClientIp(req, "::1"));
	}

	{
		// Outside the allow-list even though the family matches.
		auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
		req.set("X-Forwarded-For", "2001:db8::5");
		BOOST_CHECK_EQUAL("2001:db8::99", HTTP::Routing::EffectiveClientIp(req, "2001:db8::99"));
	}
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_EffectiveClientIp_TrimsForwardedEntry, RoutingFixture)
{
	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "10.0.0.0/8" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	{
		auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
		req.set("X-Forwarded-For", "\t 198.51.100.20 \t, 10.0.0.1");
		BOOST_CHECK_EQUAL("198.51.100.20", HTTP::Routing::EffectiveClientIp(req, "10.0.0.1"));
	}

	{
		// A first hop that is nothing but whitespace is unusable -> peer wins.
		auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
		req.set("X-Forwarded-For", " \t , 198.51.100.20");
		BOOST_CHECK_EQUAL("10.0.0.1", HTTP::Routing::EffectiveClientIp(req, "10.0.0.1"));
	}
}

//=============================================================================
// routing.cpp -- WebSocket accept / upgrade
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_WsOnAccept_TargetArms, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbWebSocket>());

	// Unparseable target.
	BOOST_CHECK_EQUAL(nullptr, HTTP::Routing::WS_OnAccept("http://example.com/rb/ws/feed"));

	// Parseable but unregistered.
	BOOST_CHECK_EQUAL(nullptr, HTTP::Routing::WS_OnAccept("/rb/ws/nothing-here"));

	// Registered - and a query string must not defeat the match.
	BOOST_CHECK_NE(nullptr, HTTP::Routing::WS_OnAccept("/rb/ws/feed"));
	BOOST_CHECK_NE(nullptr, HTTP::Routing::WS_OnAccept("/rb/ws/feed?since=1"));
}

namespace
{

	HTTP::Request MakeUpgradeRequest(std::string_view target, std::string_view subprotocol = {})
	{
		auto req = MakeRequest(boost::beast::http::verb::get, target);
		if (!subprotocol.empty())
		{
			req.set(boost::beast::http::field::sec_websocket_protocol, std::string{ subprotocol });
		}
		return req;
	}

}
// unnamed namespace

// The shared-token upgrade path accepts the credential as a `bearer.<token>`
// subprotocol entry - including one padded with the optional whitespace the
// header's ABNF permits around list members.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_WsUpgrade_SubprotocolWhitespaceTolerated, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbWebSocket>());

	HTTP::Routing::SecurityConfig config;
	config.AuthToken = "ws-token";
	HTTP::Routing::SetSecurityConfig(std::move(config));

	{
		auto req = MakeUpgradeRequest("/rb/ws/feed", "aqualink,\t bearer.ws-token \t,trailing");
		BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(req, "192.0.2.50").has_value());
	}

	{
		// A list whose LAST entry is the credential (no trailing member).
		auto req = MakeUpgradeRequest("/rb/ws/feed", "aqualink, bearer.ws-token");
		BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(req, "192.0.2.51").has_value());
	}

	{
		auto req = MakeUpgradeRequest("/rb/ws/feed", "aqualink, bearer.not-the-token ");
		auto rejection = HTTP::Routing::AuthorizeWebSocketUpgrade(req, "192.0.2.52");
		BOOST_REQUIRE(rejection.has_value());
		BOOST_CHECK(boost::beast::http::status::unauthorized == rejection->result());
	}
}

// Under auth-mode a permitted upgrade also yields a REVALIDATOR closure so the
// live socket is re-checked; a rejected upgrade must leave none behind.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_WsUpgrade_RevalidatorTracksResolver, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbWebSocket>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	HTTP::Routing::SetSecurityConfig(std::move(config));

	// The resolver honours whatever credential the upgrade carried.
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request& req, bool is_ws)
		{
			BOOST_CHECK(is_ws);
			const auto it = req.find(boost::beast::http::field::sec_websocket_protocol);
			const bool has_credential = (it != req.end()) && (std::string(it->value()).find("bearer.good") != std::string::npos);
			return MakeSubject(has_credential, has_credential ? std::vector<std::string>{ "equipment.view" } : std::vector<std::string>{});
		});

	{
		auto req = MakeUpgradeRequest("/rb/ws/feed", "aqualink, bearer.good-token");
		BOOST_REQUIRE(!HTTP::Routing::AuthorizeWebSocketUpgrade(req).has_value());

		auto revalidator = HTTP::Routing::CurrentWebSocketRevalidator();
		BOOST_REQUIRE(static_cast<bool>(revalidator));

		// The captured credential still resolves to an entitled subject.
		BOOST_CHECK(revalidator());

		// Swap in a resolver that denies everything: the SAME socket now fails
		// revalidation (this is how revocation reaches an open connection).
		HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(false, {}); });
		BOOST_CHECK(!revalidator());

		// With the whole identity system torn down, revalidation is a no-op.
		HTTP::Routing::Clear();
		BOOST_CHECK(revalidator());
	}
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_WsUpgrade_UnentitledSubjectRejected, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbWebSocket>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	HTTP::Routing::SetSecurityConfig(std::move(config));
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(true, { "prefs.self" }); });

	auto req = MakeUpgradeRequest("/rb/ws/feed");
	auto rejection = HTTP::Routing::AuthorizeWebSocketUpgrade(req);

	BOOST_REQUIRE(rejection.has_value());
	// Authenticated but not entitled -> 403, and no revalidator survives.
	BOOST_CHECK(boost::beast::http::status::forbidden == rejection->result());
	BOOST_CHECK(!static_cast<bool>(HTTP::Routing::CurrentWebSocketRevalidator()));
}

// An upgrade to a target with NO registered socket still resolves the subject
// but declares no requirement, so it is permitted with no revalidator.
BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_WsUpgrade_UnknownSocketNoRevalidator, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbWebSocket>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	HTTP::Routing::SetSecurityConfig(std::move(config));
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(true, { "equipment.view" }); });

	auto req = MakeUpgradeRequest("/rb/ws/unknown");

	BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(req).has_value());
	BOOST_CHECK(!static_cast<bool>(HTTP::Routing::CurrentWebSocketRevalidator()));
}

//=============================================================================
// routing.cpp -- CurrentSubject / CurrentPeerIp exposure
//=============================================================================

BOOST_FIXTURE_TEST_CASE(Test_RoutingBranches_CurrentSubjectAndPeerIpReflectLastRequest, RoutingFixture)
{
	HTTP::Routing::Add(std::make_unique<RbOkRoute>());

	HTTP::Routing::SecurityConfig config;
	config.AuthModeEnabled = true;
	config.TrustedProxyCidrs = { "10.0.0.0/8" };
	HTTP::Routing::SetSecurityConfig(std::move(config));
	HTTP::Routing::SetSubjectResolver([](const HTTP::Request&, bool) { return MakeSubject(true, { "equipment.view" }); });

	auto req = MakeRequest(boost::beast::http::verb::get, "/rb/ping");
	req.set("X-Forwarded-For", "198.51.100.77");

	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(req, "10.0.0.1").result());

	BOOST_CHECK_EQUAL("rb-user", HTTP::Routing::CurrentSubject().Id);
	BOOST_CHECK(HTTP::Routing::CurrentSubject().Authenticated);
	BOOST_CHECK_EQUAL("198.51.100.77", std::string(HTTP::Routing::CurrentPeerIp()));
}

BOOST_AUTO_TEST_SUITE_END()
