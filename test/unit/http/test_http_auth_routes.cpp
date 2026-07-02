#include <boost/test/unit_test.hpp>

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

#include "auth/audit_log.h"
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
#include "http/webroute_auth_login.h"
#include "http/webroute_auth_logout.h"
#include "http/webroute_auth_refresh.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;

//=============================================================================
// End-to-end auth flows THROUGH the routing layer (docs/auth-redesign.md §6):
// the deferred-response login route (argon2 on the OffloadPool via
// HTTP_OnRequestDispatch), refresh rotation, logout, and the integration
// loop — a minted access token authorising a PDP-gated control route.
//=============================================================================

inline constexpr char GATED_ROUTE_URL[] = "/api/test/gated";

namespace
{
	namespace fs = std::filesystem;

	class TestGatedRoute final : public Interfaces::IWebRoute<GATED_ROUTE_URL>
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

	struct AuthRoutesFixture
	{
		AuthRoutesFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-auth-routes-{}", counter++);
			fs::create_directories(Dir);

			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(Dir / "sessions.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{});

			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{});

			Auth::SessionService::Config config;
			config.HashParams = Auth::PasswordHasher::TestParams();

			Service = std::make_unique<Auth::SessionService>(Users, Groups, Sessions, Codec, Offload, *Audit, std::move(config));

			std::string error;
			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = Auth::PasswordHasher::Hash("correct-horse-battery", Auth::PasswordHasher::TestParams());
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogin>(*Service, IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthRefresh>(*Service));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogout>(*Service));
			HTTP::Routing::Add(std::make_unique<TestGatedRoute>());

			HTTP::Routing::SecurityConfig security;
			security.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(security));

			HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = Groups->SharedRegistry(),
				.Codec = Codec,
				.Users = Users }));
		}

		~AuthRoutesFixture()
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

		fs::path Dir;
		boost::asio::io_context IoContext;
		Utility::OffloadPool Offload{ 1 };

		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::SessionStore> Sessions;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::unique_ptr<Auth::AuditLog> Audit;
		std::unique_ptr<Auth::SessionService> Service;

		std::optional<HTTP::Message> Serialised;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpAuthRoutes)

BOOST_FIXTURE_TEST_CASE(Test_AuthRoutes_LoginToControlLoop, AuthRoutesFixture)
{
	// Without credentials the gated route answers 401 (guest, no grants).
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(boost::beast::http::verb::get, "/api/test/gated")).result());

	// Login (deferred-response route: argon2 runs on the pool, completion
	// pumped through the io_context — the production dispatch path).
	const auto login_resp = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } }));
	BOOST_REQUIRE(boost::beast::http::status::ok == login_resp.result());

	const auto tokens = BodyOf(login_resp);
	BOOST_REQUIRE(tokens.contains("access_token"));
	BOOST_REQUIRE(tokens.contains("refresh_token"));

	// The minted access token opens the PDP-gated route: the full loop.
	BOOST_CHECK(boost::beast::http::status::ok == Dispatch(MakeRequest(boost::beast::http::verb::get, "/api/test/gated", {}, tokens["access_token"].get<std::string>())).result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthRoutes_BadCredentialsAndBadBody, AuthRoutesFixture)
{
	const auto wrong = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "alice" }, { "password", "nope" } }));
	BOOST_CHECK(boost::beast::http::status::unauthorized == wrong.result());

	const auto unknown = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "mallory" }, { "password", "nope" } }));
	BOOST_CHECK(boost::beast::http::status::unauthorized == unknown.result());

	// Identical bodies: no account enumeration through the route either.
	BOOST_CHECK(BodyOf(wrong) == BodyOf(unknown));

	const auto malformed = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "user", "alice" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == malformed.result());

	const auto wrong_method = Dispatch(MakeRequest(boost::beast::http::verb::get, "/api/auth/login"));
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == wrong_method.result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthRoutes_RefreshRotationAndLogout, AuthRoutesFixture)
{
	const auto login = BodyOf(Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } })));
	const auto original_refresh = login["refresh_token"].get<std::string>();

	// Rotate.
	const auto refresh_resp = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/refresh", { { "refresh_token", original_refresh } }));
	BOOST_REQUIRE(boost::beast::http::status::ok == refresh_resp.result());
	const auto rotated = BodyOf(refresh_resp);

	// Replay of the rotated-out token: 401 (and the session is revoked).
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/refresh", { { "refresh_token", original_refresh } })).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/refresh", { { "refresh_token", rotated["refresh_token"].get<std::string>() } })).result());

	// Fresh login, then logout; the refresh token dies with the session.
	const auto second = BodyOf(Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } })));
	const auto second_refresh = second["refresh_token"].get<std::string>();

	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/logout", { { "refresh_token", second_refresh } })).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/refresh", { { "refresh_token", second_refresh } })).result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthRoutes_RevocationLocksOutLiveToken, AuthRoutesFixture)
{
	const auto login = BodyOf(Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } })));
	const auto access = login["access_token"].get<std::string>();

	BOOST_CHECK(boost::beast::http::status::ok == Dispatch(MakeRequest(boost::beast::http::verb::get, "/api/test/gated", {}, access)).result());

	// logout-everywhere (authenticated via the live access token).
	const auto everywhere = Dispatch(MakeRequest(boost::beast::http::verb::post, "/api/auth/logout", { { "refresh_token", login["refresh_token"].get<std::string>() }, { "everywhere", true } }, access));
	BOOST_CHECK(boost::beast::http::status::no_content == everywhere.result());

	// D15 immediate propagation: the still-unexpired access token is now
	// stale (tokver bumped) and the gated route answers 401 again.
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(boost::beast::http::verb::get, "/api/test/gated", {}, access)).result());
}

BOOST_AUTO_TEST_SUITE_END()
