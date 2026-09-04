#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
#include "auth/kiosk_service.h"
#include "auth/kiosk_store.h"
#include "auth/password_hasher.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "http/webroute_auth_login.h"
#include "http/webroute_auth_logout.h"
#include "http/webroute_auth_pin.h"
#include "http/webroute_auth_setup.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;

//=============================================================================
// Branch coverage for the auth minting/ending routes THROUGH the routing layer:
// every validation rejection of /api/auth/setup, /api/auth/pin (kiosk PIN
// elevation), /api/auth/login and /api/auth/logout, the lockout (429) legs of
// both deferred-response logins, the setup completion-time re-check (the
// serialisation point against a racing setup attempt), and the unreached
// synchronous OnRequest() fallbacks of the async routes.
//=============================================================================

inline constexpr char BRANCH_GATED_ROUTE_URL[] = "/api/test/gated";

namespace
{
	namespace fs = std::filesystem;

	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;

	class TestGatedRoute final : public Interfaces::IWebRoute<BRANCH_GATED_ROUTE_URL>
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

	struct AuthBranchesFixture
	{
		AuthBranchesFixture() :
			AuthBranchesFixture(true)
		{
		}

		explicit AuthBranchesFixture(bool seed_admin)
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-auth-branches-{}", counter++);
			fs::create_directories(Dir);

			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(Dir / "sessions.json"));
			Kiosk = std::make_shared<Auth::KioskStore>(Auth::KioskStore::Load(Dir / "kiosk.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{});

			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{});

			// The kiosk target group: a household scope that can view equipment.
			{
				std::string error;
				Auth::Group household{ .Name = "Household", .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" }) };
				BOOST_REQUIRE_MESSAGE(Groups->Upsert(std::move(household), error), error);
			}

			Auth::SessionService::Config session_config;
			session_config.HashParams = Auth::PasswordHasher::TestParams();
			session_config.MaxFailuresPerAccount = 2;

			Service = std::make_unique<Auth::SessionService>(Users, Groups, Sessions, Codec, Offload, *Audit, std::move(session_config));

			Auth::KioskService::Config kiosk_config;
			kiosk_config.HashParams = Auth::PasswordHasher::TestParams();
			kiosk_config.MinPinLength = 4;
			kiosk_config.MaxFailures = 2;

			KioskSvc = std::make_unique<Auth::KioskService>(Kiosk, Groups, Sessions, Codec, Offload, *Audit, std::move(kiosk_config));

			if (seed_admin)
			{
				std::string error;
				Auth::UserRecord alice;
				alice.Username = "alice";
				alice.PasswordHash = Auth::PasswordHasher::Hash("correct-horse-battery", Auth::PasswordHasher::TestParams());
				alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
				BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);
			}

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogin>(*Service, IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogout>(*Service));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthSetup>(*Users, *Audit, Offload, Auth::PasswordHasher::TestParams(), IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthPin>(*KioskSvc, IoContext.get_executor()));
			HTTP::Routing::Add(std::make_unique<TestGatedRoute>());

			HTTP::Routing::SecurityConfig security;
			security.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(security));

			HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = Groups->SharedRegistry(),
				.Codec = Codec,
				.Users = Users,
				.Kiosk = Kiosk }));
		}

		~AuthBranchesFixture()
		{
			HTTP::Routing::Clear();

			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, std::string_view raw_body = {}, std::string_view bearer = {})
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

			if (!raw_body.empty())
			{
				req.set(boost::beast::http::field::content_type, "application/json");
				req.body() = std::string{ raw_body };
				req.prepare_payload();
			}

			return req;
		}

		HTTP::Request MakeJsonRequest(boost::beast::http::verb method, std::string_view target, const nlohmann::json& body, std::string_view bearer = {})
		{
			return MakeRequest(method, target, body.dump(), bearer);
		}

		// Drive a request through the completion-based dispatcher.  `between`
		// runs AFTER the synchronous prefix of the route has executed but BEFORE
		// the io_context is pumped, i.e. before any deferred completion lands:
		// the window in which a racing kernel-thread mutation is observed by the
		// completion's re-check.
		HTTP::Response Dispatch(HTTP::Request req, const std::function<void()>& between = {})
		{
			auto guard = boost::asio::make_work_guard(IoContext);

			HTTP::Routing::HTTP_OnRequestDispatch(std::move(req), "192.168.1.50",
				[&](HTTP::Message&& msg)
				{
					Serialised = std::move(msg);
					guard.reset();
				});

			if (between)
			{
				between();
			}

			IoContext.restart();
			IoContext.run();

			BOOST_REQUIRE_MESSAGE(Serialised.has_value(), "Dispatch never completed");

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

		Auth::KioskService::SetPinResult SetPin(const std::string& pin, const std::string& group)
		{
			boost::asio::io_context io;
			auto guard = boost::asio::make_work_guard(io);

			Auth::KioskService::SetPinResult captured;
			KioskSvc->SetPin(pin, group, "admin-id", "10.0.0.1", io.get_executor(),
				[&](Auth::KioskService::SetPinResult r) { captured = std::move(r); guard.reset(); });
			io.run();
			return captured;
		}

		fs::path Dir;
		boost::asio::io_context IoContext;
		Utility::OffloadPool Offload{ 1 };

		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::SessionStore> Sessions;
		std::shared_ptr<Auth::KioskStore> Kiosk;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::unique_ptr<Auth::AuditLog> Audit;
		std::unique_ptr<Auth::SessionService> Service;
		std::unique_ptr<Auth::KioskService> KioskSvc;

		std::optional<HTTP::Message> Serialised;
	};

	// First-run shape: same stack, EMPTY user store.
	struct FirstRunBranchesFixture : AuthBranchesFixture
	{
		FirstRunBranchesFixture() :
			AuthBranchesFixture(false)
		{
		}
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpAuthBranches)

//-----------------------------------------------------------------------------
// /api/auth/setup -- validation rejections
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Setup_MethodNotAllowed, FirstRunBranchesFixture)
{
	const auto resp = Dispatch(MakeRequest(GET, "/api/auth/setup"));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "POST required");
	BOOST_CHECK(Users->Empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Setup_MalformedBodies, FirstRunBranchesFixture)
{
	// Not JSON at all.
	const auto garbage = Dispatch(MakeRequest(POST, "/api/auth/setup", "this is not json"));
	BOOST_CHECK(boost::beast::http::status::bad_request == garbage.result());
	BOOST_CHECK_EQUAL(BodyOf(garbage).value("error", ""), "Expected JSON body with username and password");

	// Missing password.
	const auto no_password = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "username", "owner" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == no_password.result());

	// Missing username.
	const auto no_username = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "password", "a-long-enough-password" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == no_username.result());

	// Username present but not a string.
	const auto numeric_username = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "username", 42 }, { "password", "a-long-enough-password" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == numeric_username.result());

	// Password present but not a string.
	const auto numeric_password = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "username", "owner" }, { "password", 1234 } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == numeric_password.result());

	// Nothing was created by any of the rejected attempts.
	BOOST_CHECK(Users->Empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Setup_EmptyUsernameRejected, FirstRunBranchesFixture)
{
	const auto resp = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "username", "" }, { "password", "a-long-enough-password" } }));

	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "Username is required");
	BOOST_CHECK(Users->Empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Setup_CompletionRecheckSealsAgainstRace, FirstRunBranchesFixture)
{
	// The synchronous prefix sees an EMPTY store and hands the hash to the
	// pool; before the completion lands, another actor creates the first
	// user.  The completion's re-check (the kernel-thread serialisation point)
	// must refuse, answering 403 -- never a second administrator.
	const auto resp = Dispatch(MakeJsonRequest(POST, "/api/auth/setup", { { "username", "owner" }, { "password", "a-long-enough-password" } }),
		[this]()
		{
			std::string error;
			Auth::UserRecord racer;
			racer.Username = "racer";
			racer.PasswordHash = Auth::PasswordHasher::Hash("racers-long-password", Auth::PasswordHasher::TestParams());
			racer.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(racer), error), error);
		});

	BOOST_CHECK(boost::beast::http::status::forbidden == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "Setup has already been completed");

	BOOST_CHECK_EQUAL(Users->Size(), 1u);
	BOOST_CHECK(!Users->FindByUsername("owner").has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Setup_SynchronousFallbackAnswers500, FirstRunBranchesFixture)
{
	// The router never dispatches an async route through OnRequest(); the
	// fallback exists only to satisfy the interface and must be a plain 500.
	HTTP::WebRoute_AuthSetup route{ *Users, *Audit, Offload, Auth::PasswordHasher::TestParams(), IoContext.get_executor() };

	const auto resp = route.OnRequest(MakeJsonRequest(POST, "/api/auth/setup", { { "username", "owner" }, { "password", "a-long-enough-password" } }));

	BOOST_CHECK(boost::beast::http::status::internal_server_error == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "setup is a deferred-response route");
	BOOST_CHECK(Users->Empty());
}

//-----------------------------------------------------------------------------
// /api/auth/pin -- kiosk PIN elevation
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_MethodNotAllowed, AuthBranchesFixture)
{
	const auto resp = Dispatch(MakeRequest(GET, "/api/auth/pin"));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "POST required");
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_MalformedBodies, AuthBranchesFixture)
{
	const auto garbage = Dispatch(MakeRequest(POST, "/api/auth/pin", "{not json"));
	BOOST_CHECK(boost::beast::http::status::bad_request == garbage.result());
	BOOST_CHECK_EQUAL(BodyOf(garbage).value("error", ""), "Expected JSON body with a pin");

	const auto missing = Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "code", "2468" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == missing.result());

	const auto numeric = Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", 2468 } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == numeric.result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_NotConfiguredIsUnauthorized, AuthBranchesFixture)
{
	// No PIN configured: a plausible PIN still fails (one indistinguishable 401).
	const auto resp = Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "2468" } }));

	BOOST_CHECK(boost::beast::http::status::unauthorized == resp.result());
	BOOST_CHECK(!BodyOf(resp).value("error", "").empty());
	BOOST_CHECK(!BodyOf(resp).contains("access_token"));
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_CorrectPinMintsUsableSession, AuthBranchesFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);

	// Anonymous: the gated route refuses.
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(GET, "/api/test/gated")).result());

	auto req = MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "2468" } });
	req.set(boost::beast::http::field::user_agent, "wall-tablet/1.0");   // the UA-present branch

	const auto resp = Dispatch(std::move(req));
	BOOST_REQUIRE(boost::beast::http::status::ok == resp.result());

	const auto tokens = BodyOf(resp);
	BOOST_CHECK(!tokens.value("access_token", "").empty());
	BOOST_CHECK(tokens.value("refresh_token", "").starts_with("art_"));
	BOOST_CHECK(!tokens.value("session_id", "").empty());
	BOOST_CHECK_EQUAL(tokens.value("token_type", ""), "Bearer");

	// The kiosk session appears under the shared kiosk subject id.
	BOOST_CHECK_EQUAL(Sessions->ForUser(std::string{ Auth::KioskService::SubjectId }).size(), 1u);

	// The minted access token opens the entitlement-gated route (Household
	// carries equipment.view).
	BOOST_CHECK(boost::beast::http::status::ok == Dispatch(MakeRequest(GET, "/api/test/gated", {}, tokens["access_token"].get<std::string>())).result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_WrongPinThenLockout, AuthBranchesFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);

	// First wrong attempt: a plain 401.
	const auto wrong = Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "0000" } }));
	BOOST_CHECK(boost::beast::http::status::unauthorized == wrong.result());
	BOOST_CHECK(wrong.find(boost::beast::http::field::retry_after) == wrong.end());

	// MaxFailures == 2: the second wrong attempt trips the lockout...
	Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "0000" } }));

	// ...so even the CORRECT PIN now answers 429 with a Retry-After hint.
	const auto locked = Dispatch(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "2468" } }));
	BOOST_CHECK(boost::beast::http::status::too_many_requests == locked.result());
	BOOST_REQUIRE(locked.find(boost::beast::http::field::retry_after) != locked.end());
	BOOST_CHECK_EQUAL(locked[boost::beast::http::field::retry_after], "900");
	BOOST_CHECK(!BodyOf(locked).contains("access_token"));
	BOOST_CHECK(!BodyOf(locked).value("error", "").empty());

	BOOST_CHECK(Sessions->ForUser(std::string{ Auth::KioskService::SubjectId }).empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Pin_SynchronousFallbackAnswers500, AuthBranchesFixture)
{
	HTTP::WebRoute_AuthPin route{ *KioskSvc, IoContext.get_executor() };

	const auto resp = route.OnRequest(MakeJsonRequest(POST, "/api/auth/pin", { { "pin", "2468" } }));

	BOOST_CHECK(boost::beast::http::status::internal_server_error == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "pin login is a deferred-response route");
}

//-----------------------------------------------------------------------------
// /api/auth/login -- lockout leg + synchronous fallback
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Login_LockoutAnswers429WithRetryAfter, AuthBranchesFixture)
{
	// MaxFailuresPerAccount == 2: two wrong passwords lock the account.
	auto first = MakeJsonRequest(POST, "/api/auth/login", { { "username", "alice" }, { "password", "nope" } });
	first.set(boost::beast::http::field::user_agent, "test-agent/1.0");   // the UA-present branch
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(std::move(first)).result());
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeJsonRequest(POST, "/api/auth/login", { { "username", "alice" }, { "password", "nope" } })).result());

	// Correct password, but the account is locked out: 429 + Retry-After.
	const auto locked = Dispatch(MakeJsonRequest(POST, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } }));
	BOOST_CHECK(boost::beast::http::status::too_many_requests == locked.result());
	BOOST_REQUIRE(locked.find(boost::beast::http::field::retry_after) != locked.end());
	BOOST_CHECK_EQUAL(locked[boost::beast::http::field::retry_after], "900");
	BOOST_CHECK(!BodyOf(locked).contains("access_token"));
	BOOST_CHECK(!BodyOf(locked).value("error", "").empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Login_SynchronousFallbackAnswers500, AuthBranchesFixture)
{
	HTTP::WebRoute_AuthLogin route{ *Service, IoContext.get_executor() };

	const auto resp = route.OnRequest(MakeJsonRequest(POST, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } }));

	BOOST_CHECK(boost::beast::http::status::internal_server_error == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "login is a deferred-response route");
}

//-----------------------------------------------------------------------------
// /api/auth/logout -- rejections
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Logout_MethodNotAllowed, AuthBranchesFixture)
{
	const auto resp = Dispatch(MakeRequest(GET, "/api/auth/logout"));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
	BOOST_CHECK_EQUAL(resp[boost::beast::http::field::content_type], "application/json");
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "POST required");
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Logout_MalformedBodies, AuthBranchesFixture)
{
	const auto garbage = Dispatch(MakeRequest(POST, "/api/auth/logout", "not-json"));
	BOOST_CHECK(boost::beast::http::status::bad_request == garbage.result());
	BOOST_CHECK_EQUAL(BodyOf(garbage).value("error", ""), "Expected JSON body with refresh_token");

	const auto missing = Dispatch(MakeJsonRequest(POST, "/api/auth/logout", { { "everywhere", true } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == missing.result());

	const auto numeric = Dispatch(MakeJsonRequest(POST, "/api/auth/logout", { { "refresh_token", 12345 } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == numeric.result());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthBranches_Logout_EverywhereNeedsAuthenticatedSubject, AuthBranchesFixture)
{
	const auto login = BodyOf(Dispatch(MakeJsonRequest(POST, "/api/auth/login", { { "username", "alice" }, { "password", "correct-horse-battery" } })));
	const auto refresh = login.value("refresh_token", "");
	BOOST_REQUIRE(!refresh.empty());

	const auto alice_id = Users->FindByUsername("alice")->Id;

	// A refresh token alone must NOT be able to end every other device's
	// session: everywhere without a bearer credential is refused...
	const auto anonymous = Dispatch(MakeJsonRequest(POST, "/api/auth/logout", { { "refresh_token", refresh }, { "everywhere", true } }));
	BOOST_CHECK(boost::beast::http::status::unauthorized == anonymous.result());
	BOOST_CHECK_EQUAL(BodyOf(anonymous).value("error", ""), "Authentication required for everywhere logout");

	// ...and the session it named is still alive.
	BOOST_CHECK_EQUAL(Sessions->ForUser(alice_id).size(), 1u);

	// The plain (single-session) logout stays available to the token holder.
	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeJsonRequest(POST, "/api/auth/logout", { { "refresh_token", refresh }, { "everywhere", false } })).result());
	BOOST_CHECK(Sessions->ForUser(alice_id).empty());
}

BOOST_AUTO_TEST_SUITE_END()
