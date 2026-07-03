#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/group.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"

using namespace AqualinkAutomate;

//=============================================================================
// The ABAC gate in the routing layer (docs/auth-redesign.md §4): per-route
// RequiredAccess declarations evaluated by the PolicyEngine against the
// resolved request Subject.  Covers posture preservation (auth-mode off ==
// historical behaviour), anonymous->Guest resolution, per-aux selector
// enforcement at the router, JWT-authenticated subjects, and 401-vs-403.
//=============================================================================

inline constexpr char VIEW_ROUTE_URL[] = "/api/test/state";
inline constexpr char CONTROL_ROUTE_URL[] = "/api/test/buttons/{button_id}";
inline constexpr char OPEN_ROUTE_URL[] = "/api/test/open";
inline constexpr char WS_ROUTE_URL[] = "/ws/test/feed";

namespace
{

	template<const auto& URL>
	class TestRouteBase : public Interfaces::IWebRoute<URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			HTTP::Response res{ boost::beast::http::status::ok, req.version() };
			res.keep_alive(req.keep_alive());
			res.body() = "ok";
			res.prepare_payload();
			return res;
		}
	};

	// GET -> equipment.view; anything else -> equipment.control.aux with the
	// path parameter as the resource id (mirrors the real button route).
	class TestViewRoute final : public TestRouteBase<VIEW_ROUTE_URL>
	{
	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
		}
	};

	class TestControlRoute final : public TestRouteBase<CONTROL_ROUTE_URL>
	{
	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_AUX, .ResourceKind = "aux" };
		}
	};

	// No RequiredAccess override: deliberately-open endpoint (auth/check shape).
	class TestOpenRoute final : public TestRouteBase<OPEN_ROUTE_URL>
	{
	};

	class TestWebSocket final : public Interfaces::IWebSocket<WS_ROUTE_URL>
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

	HTTP::Response RunRequest(HTTP::Request& req)
	{
		auto msg = HTTP::Routing::HTTP_OnRequest(req);

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

	HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, std::string_view bearer = {})
	{
		HTTP::Request req;
		req.version(11);
		req.method(method);
		req.target(target);
		req.set(boost::beast::http::field::host, "localhost.localdomain");

		if (!bearer.empty())
		{
			req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ bearer });
		}

		return req;
	}

	// Registers the test routes, enables auth-mode, and installs the real
	// subject resolver over a caller-shaped group registry (+ optional codec).
	struct AuthModeFixture
	{
		AuthModeFixture()
		{
			namespace fs = std::filesystem;

			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-routing-authz-{}", counter++);
			fs::create_directories(Dir);

			Groups = std::make_shared<Auth::GroupRegistry>(Auth::GroupRegistry::WithBuiltIns());
			KeyStore = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(KeyStore, Auth::JwtCodec::Config{});

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<TestViewRoute>());
			HTTP::Routing::Add(std::make_unique<TestControlRoute>());
			HTTP::Routing::Add(std::make_unique<TestOpenRoute>());
			HTTP::Routing::Add(std::make_unique<TestWebSocket>());

			HTTP::Routing::SecurityConfig config;
			config.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(config));

			HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Groups, Codec));
		}

		~AuthModeFixture()
		{
			HTTP::Routing::Clear();

			std::error_code ec;
			std::filesystem::remove_all(Dir, ec);
		}

		void GrantToGuest(std::initializer_list<const char*> entitlements)
		{
			std::vector<std::string> texts;
			for (const auto* text : entitlements) { texts.emplace_back(text); }

			Auth::Group guest{ .Name = std::string{ Auth::BuiltInGroups::GUEST }, .Entitlements = Auth::EntitlementSet::Parse(texts), .BuiltIn = true };
			Groups->Upsert(guest);
		}

		std::string MintToken(std::initializer_list<const char*> entitlements)
		{
			Auth::TokenClaims claims;
			claims.Subject = "user-1";
			claims.Provider = Auth::SubjectProvider::Local;

			for (const auto* text : entitlements) { claims.Entitlements.emplace_back(text); }

			return Codec->Sign(claims);
		}

		std::filesystem::path Dir;
		std::shared_ptr<Auth::GroupRegistry> Groups;
		std::shared_ptr<Auth::JwtKeyStore> KeyStore;
		std::shared_ptr<Auth::JwtCodec> Codec;
	};

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_RoutingAuthz)

//-----------------------------------------------------------------------------
// POSTURE: auth-mode OFF preserves historical behaviour exactly
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_RoutingAuthz_AuthModeOff_EverythingPermitted)
{
	HTTP::Routing::Clear();
	HTTP::Routing::Add(std::make_unique<TestViewRoute>());
	HTTP::Routing::Add(std::make_unique<TestControlRoute>());

	auto get_req = MakeRequest(boost::beast::http::verb::get, "/api/test/state");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(get_req).result());

	auto post_req = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(post_req).result());

	HTTP::Routing::Clear();
}

//-----------------------------------------------------------------------------
// ANONYMOUS -> GUEST (deny-by-default; grants open specific doors)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_AnonymousDeniedByDefault, AuthModeFixture)
{
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/test/state");

	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_OpenRouteRemainsOpen, AuthModeFixture)
{
	// Routes that declare no requirement (auth/check, health, version shape)
	// stay reachable anonymously even with auth-mode enabled.
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/test/open");

	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_GuestGrantOpensViewOnly, AuthModeFixture)
{
	GrantToGuest({ "equipment.view" });

	auto get_req = MakeRequest(boost::beast::http::verb::get, "/api/test/state");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(get_req).result());

	// Control remains denied - and as anonymous, the answer is 401 (login may elevate).
	auto post_req = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1");
	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(post_req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_GuestPerAuxSelectorEnforcedAtRouter, AuthModeFixture)
{
	GrantToGuest({ "equipment.view", "equipment.control.aux:AUX5" });

	// The granted aux toggles...
	auto granted = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX5");
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(granted).result());

	// ...its neighbour does not.
	auto denied = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1");
	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(denied).result());
}

//-----------------------------------------------------------------------------
// AUTHENTICATED SUBJECTS (JWT) - entitlement gate + 403 semantics
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_JwtSubjectPermittedByEntitlement, AuthModeFixture)
{
	const auto token = MintToken({ "equipment.view", "equipment.control.aux:*" });

	auto get_req = MakeRequest(boost::beast::http::verb::get, "/api/test/state", token);
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(get_req).result());

	auto post_req = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1", token);
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(post_req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_AuthenticatedButNotEntitledIs403, AuthModeFixture)
{
	const auto token = MintToken({ "equipment.view" });

	// Logged in, allowed to look, not allowed to touch: 403 (not 401).
	auto post_req = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1", token);
	BOOST_CHECK(boost::beast::http::status::forbidden == RunRequest(post_req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_SystemAdminIsSuperuser, AuthModeFixture)
{
	const auto token = MintToken({ "system.admin" });

	auto post_req = MakeRequest(boost::beast::http::verb::post, "/api/test/buttons/AUX1", token);
	BOOST_CHECK(boost::beast::http::status::ok == RunRequest(post_req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_GarbageTokenDegradesToAnonymous, AuthModeFixture)
{
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/test/state", "not-a-jwt");

	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(req).result());
}

//-----------------------------------------------------------------------------
// UNKNOWN /api PATHS: 401 for unauthenticated subjects (no route enumeration)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_UnknownApiPathIs401WhenAnonymous, AuthModeFixture)
{
	auto req = MakeRequest(boost::beast::http::verb::get, "/api/does/not/exist");

	BOOST_CHECK(boost::beast::http::status::unauthorized == RunRequest(req).result());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_UnknownApiPathIs404WhenAuthenticated, AuthModeFixture)
{
	const auto token = MintToken({ "equipment.view" });

	auto req = MakeRequest(boost::beast::http::verb::get, "/api/does/not/exist", token);

	BOOST_CHECK(boost::beast::http::status::not_found == RunRequest(req).result());
}

//-----------------------------------------------------------------------------
// WEBSOCKET UPGRADE GATE
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_WebSocketUpgradeGated, AuthModeFixture)
{
	auto req = MakeRequest(boost::beast::http::verb::get, "/ws/test/feed");

	// Anonymous with no guest grants: upgrade rejected 401.
	const auto denial = HTTP::Routing::AuthorizeWebSocketUpgrade(req);
	BOOST_REQUIRE(denial.has_value());
	BOOST_CHECK(boost::beast::http::status::unauthorized == denial->result());

	// Guest granted view: upgrade permitted.
	GrantToGuest({ "equipment.view" });
	BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(req).has_value());

	// Authenticated subject with view: also permitted.
	auto authed = MakeRequest(boost::beast::http::verb::get, "/ws/test/feed", MintToken({ "equipment.view" }));
	BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(authed).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_WebSocketRevalidatorClosesOnRevocation, AuthModeFixture)
{
	// Re-install the resolver over a user store so tokver revocation applies.
	auto users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));

	std::string error;
	Auth::UserRecord alice;
	alice.Username = "alice";
	alice.PasswordHash = "$argon2id$fake";
	alice.DirectEntitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
	BOOST_REQUIRE(users->Create(std::move(alice), error));
	const auto alice_id = users->FindByUsername("alice")->Id;

	HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
		.Groups = Groups, .Codec = Codec, .Users = users }));

	// Mint a token whose tokver matches the store, then upgrade.
	Auth::TokenClaims claims;
	claims.Subject = alice_id;
	claims.Provider = Auth::SubjectProvider::Local;
	claims.TokenVersion = users->FindById(alice_id)->TokenVersion;
	claims.Entitlements = { "equipment.view" };
	const auto token = Codec->Sign(claims);

	auto req = MakeRequest(boost::beast::http::verb::get, "/ws/test/feed", token);
	BOOST_REQUIRE(!HTTP::Routing::AuthorizeWebSocketUpgrade(req).has_value());

	// The revalidator captured for this connection currently passes...
	const auto revalidator = HTTP::Routing::CurrentWebSocketRevalidator();
	BOOST_REQUIRE(static_cast<bool>(revalidator));
	BOOST_CHECK(revalidator());

	// ...but after a tokver bump (logout-all / disable / entitlement change),
	// the same live connection re-checks stale and must be closed.
	users->BumpTokenVersion(alice_id);
	BOOST_CHECK(!revalidator());
}

BOOST_FIXTURE_TEST_CASE(Test_RoutingAuthz_WebSocketRevalidatorEmptyWhenAuthModeOff, AuthModeFixture)
{
	// Auth-mode off: no revalidation, sockets live for their natural lifetime.
	HTTP::Routing::SecurityConfig off;   // AuthModeEnabled defaults false
	HTTP::Routing::SetSecurityConfig(std::move(off));

	auto req = MakeRequest(boost::beast::http::verb::get, "/ws/test/feed");
	BOOST_CHECK(!HTTP::Routing::AuthorizeWebSocketUpgrade(req).has_value());
	BOOST_CHECK(!static_cast<bool>(HTTP::Routing::CurrentWebSocketRevalidator()));
}

//-----------------------------------------------------------------------------
// TRUSTED-PROXY CLIENT IDENTITY (X-Forwarded-For)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_RoutingAuthz_XffIgnoredWithoutTrustedProxies)
{
	HTTP::Routing::Clear();

	auto req = MakeRequest(boost::beast::http::verb::get, "/api/test/state");
	req.set("X-Forwarded-For", "203.0.113.7");

	// Default config: no trusted proxies -> the peer address stands.
	BOOST_CHECK_EQUAL(HTTP::Routing::EffectiveClientIp(req, "192.168.1.50"), "192.168.1.50");
}

BOOST_AUTO_TEST_CASE(Test_RoutingAuthz_XffHonouredFromTrustedProxy)
{
	HTTP::Routing::Clear();

	HTTP::Routing::SecurityConfig config;
	config.TrustedProxyCidrs = { "10.0.0.0/8" };
	HTTP::Routing::SetSecurityConfig(std::move(config));

	auto req = MakeRequest(boost::beast::http::verb::get, "/api/test/state");
	req.set("X-Forwarded-For", "203.0.113.7, 10.0.0.2");

	// Trusted proxy peer -> the FIRST forwarded hop is the client.
	BOOST_CHECK_EQUAL(HTTP::Routing::EffectiveClientIp(req, "10.0.0.2"), "203.0.113.7");

	// Same header from an UNTRUSTED peer is attacker-controlled: ignored.
	BOOST_CHECK_EQUAL(HTTP::Routing::EffectiveClientIp(req, "192.168.1.66"), "192.168.1.66");

	HTTP::Routing::Clear();
}

BOOST_AUTO_TEST_SUITE_END()
