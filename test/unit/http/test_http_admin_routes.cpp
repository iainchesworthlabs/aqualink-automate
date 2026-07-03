#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/password_hasher.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "http/webroute_apikey.h"
#include "http/webroute_apikeys.h"
#include "http/webroute_auth_login.h"
#include "http/webroute_auth_refresh.h"
#include "http/webroute_entitlements.h"
#include "http/webroute_group.h"
#include "http/webroute_groups.h"
#include "http/webroute_session.h"
#include "http/webroute_sessions.h"
#include "http/webroute_user.h"
#include "http/webroute_user_password.h"
#include "http/webroute_users.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;

//=============================================================================
// The admin/user-management API surface THROUGH the routing layer (docs/auth-
// redesign.md §6-§7): user CRUD (incl. the deferred-response create/password
// flows), group upserts with D15 tokver propagation, the entitlement
// vocabulary, API keys (shown-once secret, revocation kills the resolver
// path) and the session list / per-session revoke — for an admin subject, a
// non-admin subject (self-scope only) and the anonymous subject.
//=============================================================================

inline constexpr char ADMIN_GATED_ROUTE_URL[] = "/api/test/gated";

namespace
{
	namespace fs = std::filesystem;

	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;
	constexpr auto PUT = boost::beast::http::verb::put;
	constexpr auto DELETE_ = boost::beast::http::verb::delete_;

	// Entitlement-gated probe: proves an API key's grant works through the
	// resolver, and that revocation kills it.
	class TestGatedRoute final : public Interfaces::IWebRoute<ADMIN_GATED_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) override
		{
			HTTP::Response res{ boost::beast::http::status::ok, req.version() };
			res.keep_alive(req.keep_alive());
			res.body() = "granted";
			res.prepare_payload();
			return res;
		}

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = "equipment.view" };
		}
	};

	struct AdminRoutesFixture
	{
		AdminRoutesFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-admin-routes-{}", counter++);
			fs::create_directories(Dir);

			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(Dir / "sessions.json"));
			ApiKeys = std::make_shared<Auth::ApiKeyStore>(Auth::ApiKeyStore::Load(Dir / "api-keys.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{});

			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{});

			Auth::SessionService::Config config;
			config.HashParams = Auth::PasswordHasher::TestParams();

			Service = std::make_unique<Auth::SessionService>(Users, Groups, Sessions, Codec, Offload, *Audit, std::move(config));

			// alice: administrator.  bob: group-less standard user with a
			// single direct grant (equipment.view) — NOT an admin.
			std::string error;

			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = Auth::PasswordHasher::Hash("correct-horse-battery", Auth::PasswordHasher::TestParams());
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);

			Auth::UserRecord bob;
			bob.Username = "bob";
			bob.PasswordHash = Auth::PasswordHasher::Hash("bobs-long-password", Auth::PasswordHasher::TestParams());
			bob.DirectEntitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(bob), error), error);

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogin>(*Service, IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthRefresh>(*Service));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Users>(*Users, *Audit, Offload, Auth::PasswordHasher::TestParams(), IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_User>(*Users, *Groups, *Service, *Sessions, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_UserPassword>(*Users, *Groups, *Sessions, *Audit, Offload, Auth::PasswordHasher::TestParams(), IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Groups>(*Groups, *Users, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Group>(*Groups, *Users, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Entitlements>());
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKeys>(*ApiKeys, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKey>(*ApiKeys, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Sessions>(*Sessions));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Session>(*Sessions, *Audit));
			HTTP::Routing::Add(std::make_unique<TestGatedRoute>());

			HTTP::Routing::SecurityConfig security;
			security.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(security));

			HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = Groups->SharedRegistry(),
				.Codec = Codec,
				.Users = Users,
				.ApiKeys = ApiKeys }));
		}

		~AdminRoutesFixture()
		{
			HTTP::Routing::Clear();

			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, const nlohmann::json& body = {}, std::string_view bearer = {})
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

			if (!body.empty())
			{
				req.set(boost::beast::http::field::content_type, "application/json");
				req.body() = body.dump();
				req.prepare_payload();
			}

			return req;
		}

		// Drive a request through the COMPLETION-BASED dispatcher, pumping the
		// io_context until the (possibly deferred) response lands — exactly the
		// production shape: HttpSessionState -> HTTP_OnRequestDispatch -> DoWrite.
		HTTP::Response Dispatch(HTTP::Request req)
		{
			auto guard = boost::asio::make_work_guard(IoContext);

			HTTP::Routing::HTTP_OnRequestDispatch(std::move(req), "192.168.1.50",
				[&](HTTP::Message&& msg)
				{
					Serialised = std::move(msg);
					guard.reset();
				});

			IoContext.restart();
			IoContext.run();

			BOOST_REQUIRE_MESSAGE(Serialised.has_value(), "Dispatch never completed");

			// The completion hands over a type-erased message_generator; render
			// it to bytes and re-parse as a Response to assert status/body.
			boost::beast::error_code ec;
			std::string wire;

			while (!Serialised->is_done())
			{
				const auto buffers = Serialised->prepare(ec);
				BOOST_REQUIRE(!ec);

				for (const auto b : boost::beast::buffers_range_ref(buffers))
				{
					wire.append(static_cast<const char*>(b.data()), b.size());
				}

				Serialised->consume(boost::beast::buffer_bytes(buffers));
			}

			Serialised.reset();

			boost::beast::http::response_parser<boost::beast::http::string_body> parser;
			parser.eager(true);
			parser.put(boost::asio::buffer(wire), ec);
			BOOST_REQUIRE_MESSAGE(!ec, "Could not parse serialised response: " + ec.message());

			if (!parser.is_done())
			{
				parser.put_eof(ec);
				BOOST_REQUIRE_MESSAGE(!ec, "Could not finish parsing serialised response: " + ec.message());
			}

			return parser.release();
		}

		nlohmann::json BodyOf(const HTTP::Response& resp)
		{
			return nlohmann::json::parse(resp.body(), nullptr, false);
		}

		nlohmann::json Login(std::string_view username, std::string_view password)
		{
			const auto resp = Dispatch(MakeRequest(POST, "/api/auth/login", { { "username", username }, { "password", password } }));
			BOOST_REQUIRE_MESSAGE(boost::beast::http::status::ok == resp.result(), std::format("login as '{}' failed: {}", username, resp.body()));
			return BodyOf(resp);
		}

		std::string UserId(std::string_view username)
		{
			const auto user = Users->FindByUsername(username);
			BOOST_REQUIRE_MESSAGE(user.has_value(), std::format("no user '{}'", username));
			return user->Id;
		}

		fs::path Dir;
		boost::asio::io_context IoContext;
		Utility::OffloadPool Offload{ 1 };

		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::SessionStore> Sessions;
		std::shared_ptr<Auth::ApiKeyStore> ApiKeys;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::unique_ptr<Auth::AuditLog> Audit;
		std::unique_ptr<Auth::SessionService> Service;

		std::optional<HTTP::Message> Serialised;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpAdminRoutes)

//-----------------------------------------------------------------------------
// GATE SWEEP: every system.admin route answers 403 to a non-admin and 401 to
// the anonymous subject.
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_NonAdminForbiddenAnonymousUnauthorized, AdminRoutesFixture)
{
	const auto bob = Login("bob", "bobs-long-password")["access_token"].get<std::string>();
	const auto alice_id = UserId("alice");

	const struct { boost::beast::http::verb Method; std::string Target; } admin_surface[] =
	{
		{ GET, "/api/users" },
		{ POST, "/api/users" },
		{ GET, std::format("/api/users/{}", alice_id) },
		{ PUT, std::format("/api/users/{}", alice_id) },
		{ DELETE_, std::format("/api/users/{}", alice_id) },
		{ GET, "/api/groups" },
		{ POST, "/api/groups" },
		{ DELETE_, "/api/groups/Staff" },
		{ GET, "/api/entitlements" },
		{ GET, "/api/apikeys" },
		{ POST, "/api/apikeys" },
		{ DELETE_, "/api/apikeys/some-key" },
	};

	for (const auto& probe : admin_surface)
	{
		BOOST_TEST_CONTEXT(std::format("{} {}", std::string{ boost::beast::http::to_string(probe.Method) }, probe.Target))
		{
			// Authenticated but not entitled -> 403; anonymous -> 401.
			BOOST_CHECK(boost::beast::http::status::forbidden == Dispatch(MakeRequest(probe.Method, probe.Target, {}, bob)).result());
			BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(probe.Method, probe.Target)).result());
		}
	}

	// The self-scoped (PREFS_SELF) routes still refuse the anonymous subject.
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(GET, "/api/sessions")).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(DELETE_, "/api/sessions/some-session")).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(PUT, std::format("/api/users/{}/password", alice_id))).result());
}

//-----------------------------------------------------------------------------
// USER CRUD
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_UserCrudRoundTrip, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();

	// CREATE (deferred-response route: the hash runs on the pool).
	const auto created = Dispatch(MakeRequest(POST, "/api/users",
		{ { "username", "carol" }, { "password", "carols-long-password" }, { "direct_entitlements", { "equipment.view" } } }, admin));
	BOOST_REQUIRE(boost::beast::http::status::created == created.result());

	const auto carol = BodyOf(created);
	BOOST_REQUIRE(carol.contains("id"));
	BOOST_CHECK_EQUAL(carol["username"].get<std::string>(), "carol");
	BOOST_CHECK(!created.body().contains("password"));

	// Weak password -> 400; duplicate username -> 409.
	BOOST_CHECK(boost::beast::http::status::bad_request == Dispatch(MakeRequest(POST, "/api/users", { { "username", "dave" }, { "password", "weak" } }, admin)).result());
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(POST, "/api/users", { { "username", "carol" }, { "password", "another-long-password" } }, admin)).result());

	// LIST: three users, and NEVER any password material.
	const auto list_resp = Dispatch(MakeRequest(GET, "/api/users", {}, admin));
	BOOST_REQUIRE(boost::beast::http::status::ok == list_resp.result());
	const auto list = BodyOf(list_resp);
	BOOST_CHECK_EQUAL(list.size(), 3u);
	BOOST_CHECK(std::string::npos == list_resp.body().find("password"));
	BOOST_CHECK(std::string::npos == list_resp.body().find("argon2"));

	// GET one / 404 unknown.
	const auto carol_id = carol["id"].get<std::string>();
	BOOST_CHECK(boost::beast::http::status::ok == Dispatch(MakeRequest(GET, std::format("/api/users/{}", carol_id), {}, admin)).result());
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(GET, "/api/users/no-such-user", {}, admin)).result());

	// UPDATE: an entitlement change bumps tokver ONLY (sessions survive; the
	// still-valid refresh token re-mints with the new grants).
	Login("carol", "carols-long-password");
	const auto tokver_before = Users->FindById(carol_id)->TokenVersion;
	const auto sessions_before = Sessions->ForUser(carol_id).size();

	const auto updated = Dispatch(MakeRequest(PUT, std::format("/api/users/{}", carol_id), { { "direct_entitlements", { "equipment.view", "schedules.view" } } }, admin));
	BOOST_REQUIRE(boost::beast::http::status::ok == updated.result());
	BOOST_CHECK(Users->FindById(carol_id)->TokenVersion > tokver_before);
	BOOST_CHECK_EQUAL(Sessions->ForUser(carol_id).size(), sessions_before);

	// disabled=true routes through SessionService::DisableUser: sessions die.
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(PUT, std::format("/api/users/{}", carol_id), { { "disabled", true } }, admin)).result());
	BOOST_CHECK(Users->FindById(carol_id)->Disabled);
	BOOST_CHECK(Sessions->ForUser(carol_id).empty());

	// DELETE.
	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, std::format("/api/users/{}", carol_id), {}, admin)).result());
	BOOST_CHECK(!Users->FindById(carol_id).has_value());
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(DELETE_, std::format("/api/users/{}", carol_id), {}, admin)).result());
}

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_LastAdminProtection, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();
	const auto alice_id = UserId("alice");

	// alice is the ONLY admin: disable, de-admin and delete are all refused.
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(PUT, std::format("/api/users/{}", alice_id), { { "disabled", true } }, admin)).result());
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(PUT, std::format("/api/users/{}", alice_id), { { "groups", nlohmann::json::array() } }, admin)).result());
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(DELETE_, std::format("/api/users/{}", alice_id), {}, admin)).result());

	BOOST_CHECK(Users->FindById(alice_id).has_value());
	BOOST_CHECK(!Users->FindById(alice_id)->Disabled);
}

//-----------------------------------------------------------------------------
// PASSWORD LIFECYCLE (/api/users/{user_id}/password)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_PasswordChangeSelfVsOther, AdminRoutesFixture)
{
	const auto bob_login = Login("bob", "bobs-long-password");
	const auto bob_access = bob_login["access_token"].get<std::string>();
	const auto bob_refresh = bob_login["refresh_token"].get<std::string>();
	const auto bob_id = UserId("bob");
	const auto alice_id = UserId("alice");

	// bob may NOT set alice's password (403; self-scope only).
	BOOST_CHECK(boost::beast::http::status::forbidden == Dispatch(MakeRequest(PUT, std::format("/api/users/{}/password", alice_id), { { "password", "hijacked-password!" } }, bob_access)).result());

	// Weak replacement refused.
	BOOST_CHECK(boost::beast::http::status::bad_request == Dispatch(MakeRequest(PUT, std::format("/api/users/{}/password", bob_id), { { "password", "short" } }, bob_access)).result());

	// bob changes his OWN password (deferred-response route) -> 204, and the
	// change invalidates EVERYTHING he held.
	BOOST_REQUIRE(boost::beast::http::status::no_content == Dispatch(MakeRequest(PUT, std::format("/api/users/{}/password", bob_id), { { "password", "bobs-new-long-password" } }, bob_access)).result());

	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(POST, "/api/auth/refresh", { { "refresh_token", bob_refresh } })).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(GET, "/api/test/gated", {}, bob_access)).result());

	// Old password dead, new one lives.
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(POST, "/api/auth/login", { { "username", "bob" }, { "password", "bobs-long-password" } })).result());
	Login("bob", "bobs-new-long-password");
}

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_AdminResetsAnotherUsersPassword, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();
	const auto bob_id = UserId("bob");

	BOOST_REQUIRE(boost::beast::http::status::no_content == Dispatch(MakeRequest(PUT, std::format("/api/users/{}/password", bob_id), { { "password", "admin-issued-password" } }, admin)).result());

	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(POST, "/api/auth/login", { { "username", "bob" }, { "password", "bobs-long-password" } })).result());
	Login("bob", "admin-issued-password");

	// Unknown target -> 404 (admin scope, so past the self-check).
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(PUT, "/api/users/no-such-user/password", { { "password", "whatever-long-enough" } }, admin)).result());
}

//-----------------------------------------------------------------------------
// GROUPS
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_GroupUpsertValidationAndPropagation, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();

	// An unknown/malformed entitlement is refused, and NAMED in the error.
	const auto rejected = Dispatch(MakeRequest(POST, "/api/groups", { { "name", "Staff" }, { "entitlements", { "equipment.view", "bogus.notanaction" } } }, admin));
	BOOST_REQUIRE(boost::beast::http::status::bad_request == rejected.result());
	BOOST_CHECK(std::string::npos != BodyOf(rejected)["error"].get<std::string>().find("bogus.notanaction"));
	BOOST_CHECK(!Groups->Registry().Find("Staff").has_value());

	// Valid upsert.
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(POST, "/api/groups", { { "name", "Staff" }, { "entitlements", { "equipment.view" } } }, admin)).result());

	// Enrol bob, then CHANGE the group's entitlements: D15 — bob's tokver
	// bumps so his outstanding access tokens go stale within a request.
	const auto bob_id = UserId("bob");
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(PUT, std::format("/api/users/{}", bob_id), { { "groups", { "Staff" } } }, admin)).result());

	const auto tokver_before = Users->FindById(bob_id)->TokenVersion;

	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(POST, "/api/groups", { { "name", "Staff" }, { "entitlements", { "equipment.view", "schedules.view" } } }, admin)).result());
	BOOST_CHECK(Users->FindById(bob_id)->TokenVersion > tokver_before);

	// An upsert with UNCHANGED entitlements does not churn tokver.
	const auto tokver_stable = Users->FindById(bob_id)->TokenVersion;
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(POST, "/api/groups", { { "name", "Staff" }, { "entitlements", { "equipment.view", "schedules.view" } } }, admin)).result());
	BOOST_CHECK_EQUAL(Users->FindById(bob_id)->TokenVersion, tokver_stable);

	// The list shows the built-ins plus Staff.
	const auto list = BodyOf(Dispatch(MakeRequest(GET, "/api/groups", {}, admin)));
	BOOST_CHECK_EQUAL(list.size(), 4u);
}

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_GroupDeleteBuiltInsProtected, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();

	// Built-ins are undeletable -> 409; unknown -> 404.
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(DELETE_, "/api/groups/Administrators", {}, admin)).result());
	BOOST_CHECK(boost::beast::http::status::conflict == Dispatch(MakeRequest(DELETE_, "/api/groups/Guest", {}, admin)).result());
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(DELETE_, "/api/groups/NoSuchGroup", {}, admin)).result());

	// A real group deletes; members' tokver bumps (they LOST the grants).
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(POST, "/api/groups", { { "name", "Staff" }, { "entitlements", { "equipment.view" } } }, admin)).result());

	const auto bob_id = UserId("bob");
	BOOST_REQUIRE(boost::beast::http::status::ok == Dispatch(MakeRequest(PUT, std::format("/api/users/{}", bob_id), { { "groups", { "Staff" } } }, admin)).result());

	const auto tokver_before = Users->FindById(bob_id)->TokenVersion;

	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, "/api/groups/Staff", {}, admin)).result());
	BOOST_CHECK(!Groups->Registry().Find("Staff").has_value());
	BOOST_CHECK(Users->FindById(bob_id)->TokenVersion > tokver_before);
}

//-----------------------------------------------------------------------------
// ENTITLEMENT VOCABULARY
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_EntitlementVocabularyEnumerable, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();

	const auto resp = Dispatch(MakeRequest(GET, "/api/entitlements", {}, admin));
	BOOST_REQUIRE(boost::beast::http::status::ok == resp.result());

	const auto actions = BodyOf(resp)["actions"];
	BOOST_CHECK_EQUAL(actions.size(), Auth::Vocabulary::ALL_ACTIONS.size());
	BOOST_CHECK(actions.end() != std::find(actions.begin(), actions.end(), "system.admin"));
	BOOST_CHECK(actions.end() != std::find(actions.begin(), actions.end(), "equipment.view"));
}

//-----------------------------------------------------------------------------
// API KEYS
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_ApiKeyLifecycle, AdminRoutesFixture)
{
	const auto admin = Login("alice", "correct-horse-battery")["access_token"].get<std::string>();

	// Unknown entitlement refused, named.
	const auto rejected = Dispatch(MakeRequest(POST, "/api/apikeys", { { "label", "ha-bridge" }, { "entitlements", { "not.real" } } }, admin));
	BOOST_REQUIRE(boost::beast::http::status::bad_request == rejected.result());
	BOOST_CHECK(std::string::npos != BodyOf(rejected)["error"].get<std::string>().find("not.real"));

	// CREATE: the ONE-TIME secret + warning come back exactly once.
	const auto created = Dispatch(MakeRequest(POST, "/api/apikeys", { { "label", "ha-bridge" }, { "entitlements", { "equipment.view" } } }, admin));
	BOOST_REQUIRE(boost::beast::http::status::created == created.result());

	const auto key = BodyOf(created);
	const auto secret = key["secret"].get<std::string>();
	const auto key_id = key["id"].get<std::string>();
	BOOST_CHECK(secret.starts_with("aak_"));
	BOOST_CHECK(key.contains("warning"));

	// LIST: never the secret, never its digest.
	const auto list_resp = Dispatch(MakeRequest(GET, "/api/apikeys", {}, admin));
	BOOST_REQUIRE(boost::beast::http::status::ok == list_resp.result());
	BOOST_CHECK_EQUAL(BodyOf(list_resp).size(), 1u);
	BOOST_CHECK(std::string::npos == list_resp.body().find(secret));
	BOOST_CHECK(std::string::npos == list_resp.body().find(Auth::ApiKeyStore::DigestOf(secret)));
	BOOST_CHECK(std::string::npos == list_resp.body().find("secret"));

	// The key's grant works through the resolver...
	BOOST_CHECK(boost::beast::http::status::ok == Dispatch(MakeRequest(GET, "/api/test/gated", {}, secret)).result());
	// ...but is scoped: no system.admin, so the admin surface answers 403.
	BOOST_CHECK(boost::beast::http::status::forbidden == Dispatch(MakeRequest(GET, "/api/users", {}, secret)).result());

	// REVOKE kills authentication immediately.
	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, std::format("/api/apikeys/{}", key_id), {}, admin)).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(GET, "/api/test/gated", {}, secret)).result());

	// Unknown id -> 404.
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(DELETE_, "/api/apikeys/no-such-key", {}, admin)).result());
}

//-----------------------------------------------------------------------------
// SESSIONS
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_SessionListScopedByRole, AdminRoutesFixture)
{
	const auto alice_login = Login("alice", "correct-horse-battery");
	const auto bob_login = Login("bob", "bobs-long-password");

	const auto alice_access = alice_login["access_token"].get<std::string>();
	const auto bob_access = bob_login["access_token"].get<std::string>();
	const auto bob_id = UserId("bob");

	// bob sees ONLY his own session.
	const auto bob_view = BodyOf(Dispatch(MakeRequest(GET, "/api/sessions", {}, bob_access)));
	BOOST_REQUIRE_EQUAL(bob_view.size(), 1u);
	BOOST_CHECK_EQUAL(bob_view[0]["user_id"].get<std::string>(), bob_id);

	// The admin sees everyone's.
	const auto admin_view = BodyOf(Dispatch(MakeRequest(GET, "/api/sessions", {}, alice_access)));
	BOOST_CHECK_EQUAL(admin_view.size(), 2u);
}

BOOST_FIXTURE_TEST_CASE(Test_AdminRoutes_SessionRevokeOwnershipRules, AdminRoutesFixture)
{
	const auto alice_login = Login("alice", "correct-horse-battery");
	const auto bob_login = Login("bob", "bobs-long-password");

	const auto alice_access = alice_login["access_token"].get<std::string>();
	const auto bob_access = bob_login["access_token"].get<std::string>();
	const auto bob_refresh = bob_login["refresh_token"].get<std::string>();

	const auto alice_id = UserId("alice");
	const auto bob_id = UserId("bob");

	BOOST_REQUIRE_EQUAL(Sessions->ForUser(alice_id).size(), 1u);
	BOOST_REQUIRE_EQUAL(Sessions->ForUser(bob_id).size(), 1u);

	const auto alice_session = Sessions->ForUser(alice_id).front().Id;
	const auto bob_session = Sessions->ForUser(bob_id).front().Id;

	// bob revoking ALICE's session: 404 — indistinguishable from an unknown
	// id, so session ids are not enumerable — and the session survives.
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(DELETE_, std::format("/api/sessions/{}", alice_session), {}, bob_access)).result());
	BOOST_CHECK_EQUAL(Sessions->ForUser(alice_id).size(), 1u);

	// bob revoking his OWN session works.
	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, std::format("/api/sessions/{}", bob_session), {}, bob_access)).result());
	BOOST_CHECK(Sessions->ForUser(bob_id).empty());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(POST, "/api/auth/refresh", { { "refresh_token", bob_refresh } })).result());

	// The admin may revoke ANYONE's: bob logs back in, alice kills it, and
	// bob's refresh token dies with it.
	const auto second_login = Login("bob", "bobs-long-password");
	const auto second_refresh = second_login["refresh_token"].get<std::string>();
	const auto second_session = Sessions->ForUser(bob_id).front().Id;

	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, std::format("/api/sessions/{}", second_session), {}, alice_access)).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(POST, "/api/auth/refresh", { { "refresh_token", second_refresh } })).result());

	// Unknown id -> 404 for the admin too.
	BOOST_CHECK(boost::beast::http::status::not_found == Dispatch(MakeRequest(DELETE_, "/api/sessions/no-such-session", {}, alice_access)).result());
}

BOOST_AUTO_TEST_SUITE_END()
