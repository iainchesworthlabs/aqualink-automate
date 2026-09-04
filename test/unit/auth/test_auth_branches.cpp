#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/password_hasher.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/server_types.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;

//=============================================================================
// Credential-handling arms that the main auth suites do not reach.
//
// Both files sit directly on the request path for every authenticated call, so
// the interesting cases are the DEGRADED ones: a header that does not carry a
// credential in the expected shape, a credential presented with no machinery
// wired to interpret it, and a session whose account disappeared underneath it.
// Every one of these must fall back to "anonymous"/"failed" rather than throw.
//=============================================================================

namespace
{

	namespace fs = std::filesystem;

	struct AuthTempDir
	{
		AuthTempDir()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-auth-branches-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~AuthTempDir()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	struct ResolverBranchFixture : AuthTempDir
	{
		ResolverBranchFixture()
		{
			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			GroupsStore = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			ApiKeys = std::make_shared<Auth::ApiKeyStore>(Auth::ApiKeyStore::Load(Dir / "api-keys.json"));

			Keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(Keys, Auth::JwtCodec::Config{});

			Resolver = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = GroupsStore->SharedRegistry(),
				.Codec = Codec,
				.Users = Users,
				.ApiKeys = ApiKeys });

			std::string error;
			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = "$argon2id$fake";
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);

			AliceId = Users->FindByUsername("alice")->Id;
		}

		std::string MintForAlice() const
		{
			const auto user = Users->FindById(AliceId);

			Auth::TokenClaims claims;
			claims.Subject = user->Id;
			claims.Provider = Auth::SubjectProvider::Local;
			claims.TokenVersion = user->TokenVersion;
			claims.Groups = user->Groups;
			claims.Entitlements = GroupsStore->Registry().ResolveEffectiveEntitlements(user->DirectEntitlements, user->Groups).ToStrings();

			return Codec->Sign(claims);
		}

		// A request carrying a VERBATIM Authorization header value.
		static HTTP::Request WithAuthorization(std::string_view raw_header)
		{
			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::get);
			req.target("/api/equipment");

			if (!raw_header.empty())
			{
				req.set(boost::beast::http::field::authorization, std::string{ raw_header });
			}

			return req;
		}

		// A WebSocket upgrade carrying a VERBATIM Sec-WebSocket-Protocol list.
		static HTTP::Request WithSubprotocols(std::string_view raw_header)
		{
			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::get);
			req.target("/ws/equipment");

			if (!raw_header.empty())
			{
				req.set(boost::beast::http::field::sec_websocket_protocol, std::string{ raw_header });
			}

			return req;
		}

		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> GroupsStore;
		std::shared_ptr<Auth::ApiKeyStore> ApiKeys;
		std::shared_ptr<Auth::JwtKeyStore> Keys;
		std::shared_ptr<Auth::JwtCodec> Codec;
		HTTP::Routing::SubjectResolver Resolver;
		std::string AliceId;
	};

	struct SessionBranchFixture : AuthTempDir
	{
		SessionBranchFixture()
		{
			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(Dir / "sessions.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{ .Now = [this]() { return Now; } });

			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{ .JsonlFile = Dir / "audit.jsonl" });

			Auth::SessionService::Config config;
			config.HashParams = Auth::PasswordHasher::TestParams();
			config.Now = [this]() { return Now; };

			Service = std::make_unique<Auth::SessionService>(Users, Groups, Sessions, Codec, Offload, *Audit, std::move(config));

			std::string error;

			Auth::UserRecord admin;
			admin.Username = "admin";
			admin.PasswordHash = Auth::PasswordHasher::Hash("admin-password-1", Auth::PasswordHasher::TestParams());
			admin.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(admin), error), error);

			Auth::UserRecord bob;
			bob.Username = "bob";
			bob.PasswordHash = Auth::PasswordHasher::Hash("bob-password-1", Auth::PasswordHasher::TestParams());
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(bob), error), error);

			BobId = Users->FindByUsername("bob")->Id;
		}

		Auth::SessionService::LoginResult Login(const std::string& username, const std::string& password)
		{
			boost::asio::io_context io_context;
			auto guard = boost::asio::make_work_guard(io_context);

			Auth::SessionService::LoginResult captured;

			Service->Login(username, password, "192.168.1.51", "test-agent", io_context.get_executor(),
				[&](Auth::SessionService::LoginResult result)
				{
					captured = std::move(result);
					guard.reset();
				});

			io_context.run();

			return captured;
		}

		std::chrono::system_clock::time_point Now{ std::chrono::system_clock::now() };

		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::SessionStore> Sessions;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::unique_ptr<Auth::AuditLog> Audit;
		Utility::OffloadPool Offload{ 1 };
		std::unique_ptr<Auth::SessionService> Service;
		std::string BobId;
	};

}
// unnamed namespace

//=============================================================================
// Subject resolution — malformed / unusable credentials
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_AuthBranches)

// The Authorization header must be exactly a `Bearer <token>` credential.
// Anything else is not a credential we understand and degrades to anonymous
// rather than being coerced into one.
BOOST_FIXTURE_TEST_CASE(AuthBranches_NonBearerAuthorizationHeaderIsAnonymous, ResolverBranchFixture)
{
	const auto token = MintForAlice();

	// Sanity: the token itself is good in the correct shape.
	{
		auto req = WithAuthorization("Bearer " + token);
		BOOST_REQUIRE(Resolver(req, false).Authenticated);
	}

	const auto is_anonymous = [this](std::string_view header)
		{
			auto req = WithAuthorization(header);
			const auto subject = Resolver(req, false);
			BOOST_CHECK_MESSAGE(!subject.Authenticated, std::string{ "unexpectedly authenticated for header: " } + std::string{ header });
			BOOST_CHECK(Auth::SubjectProvider::Anonymous == subject.Provider);
		};

	is_anonymous("Basic YWxpY2U6c2VjcmV0");
	is_anonymous("bearer " + token);    // scheme is case-sensitive here
	is_anonymous("Bearer");             // no space, no token
	is_anonymous(token);                // bare token, no scheme
	is_anonymous("Token " + token);
}

// A garbage bearer token that is neither a valid JWT nor a known API key
// resolves to the anonymous (Guest) subject, not an error.
BOOST_FIXTURE_TEST_CASE(AuthBranches_UnverifiableBearerIsAnonymous, ResolverBranchFixture)
{
	for (const char* token : { "not-a-jwt", "a.b.c", "aak_definitely-not-a-real-key", "" })
	{
		auto req = WithAuthorization(std::string{ "Bearer " } + token);
		const auto subject = Resolver(req, false);

		BOOST_CHECK(!subject.Authenticated);
		BOOST_CHECK(Auth::SubjectProvider::Anonymous == subject.Provider);
	}
}

// The WebSocket handshake carries the credential in a comma-separated
// subprotocol list.  The list is parsed per-entry with the optional whitespace
// the header's ABNF permits, and an entry that is not a bearer entry is skipped.
BOOST_FIXTURE_TEST_CASE(AuthBranches_WebSocketSubprotocolListParsing, ResolverBranchFixture)
{
	const auto token = MintForAlice();

	const auto resolve = [this](const std::string& header)
		{
			auto req = WithSubprotocols(header);
			return Resolver(req, true);
		};

	// Padded with optional whitespace on both sides, and not the first entry.
	BOOST_CHECK(resolve("aqualink,\t bearer." + token + " \t,trailing").Authenticated);

	// Last entry in the list (no trailing member to terminate it).
	BOOST_CHECK(resolve("aqualink, bearer." + token).Authenticated);

	// Only entry.
	BOOST_CHECK(resolve("bearer." + token).Authenticated);

	// A list with NO bearer entry at all -> anonymous (the parser runs off the
	// end of the list rather than mis-reading a neighbouring entry).
	BOOST_CHECK(!resolve("aqualink").Authenticated);
	BOOST_CHECK(!resolve("aqualink, something, else").Authenticated);
	BOOST_CHECK(!resolve("bearer").Authenticated);          // prefix without the dot
	BOOST_CHECK(!resolve("notbearer." + token).Authenticated);

	// An upgrade must NOT fall back to the Authorization header (a browser
	// cannot set one), so a token offered only there is ignored.
	{
		auto req = WithAuthorization("Bearer " + token);
		BOOST_CHECK(!Resolver(req, true).Authenticated);
	}
}

// With no JWT codec wired (a substrate configuration), a perfectly valid
// session token cannot be interpreted and the request is anonymous - it must
// not be trusted unverified.
BOOST_FIXTURE_TEST_CASE(AuthBranches_NoCodecWiredRejectsJwt, ResolverBranchFixture)
{
	const auto token = MintForAlice();

	auto codeless = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
		.Groups = GroupsStore->SharedRegistry(),
		.Codec = nullptr,
		.Users = Users,
		.ApiKeys = ApiKeys });

	auto req = WithAuthorization("Bearer " + token);
	const auto subject = codeless(req, false);

	BOOST_CHECK(!subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::Anonymous == subject.Provider);
}

// With no API-key store wired, a machine credential has nothing to match
// against and likewise degrades to anonymous.
BOOST_FIXTURE_TEST_CASE(AuthBranches_NoApiKeyStoreWiredRejectsMachineCredential, ResolverBranchFixture)
{
	auto keyless = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
		.Groups = GroupsStore->SharedRegistry(),
		.Codec = Codec,
		.Users = Users,
		.ApiKeys = nullptr });

	auto req = WithAuthorization("Bearer aak_some-machine-key");
	BOOST_CHECK(!keyless(req, false).Authenticated);
}

//=============================================================================
// SessionService — failure arms
//=============================================================================

// Logging out with a token that was never issued (or has already been used) is
// a no-op that reports "nothing revoked" rather than an error.
BOOST_FIXTURE_TEST_CASE(AuthBranches_LogoutUnknownRefreshTokenReturnsFalse, SessionBranchFixture)
{
	BOOST_CHECK(!Service->Logout("art_never-issued", "10.0.0.9"));
	BOOST_CHECK(!Service->Logout("", "10.0.0.9"));

	// A real session logs out once, and only once.
	const auto login = Login("bob", "bob-password-1");
	BOOST_REQUIRE(login.Success);

	BOOST_CHECK(Service->Logout(login.RefreshToken, "10.0.0.9"));
	BOOST_CHECK(!Service->Logout(login.RefreshToken, "10.0.0.9"));
}

// Disabling an account that does not exist reports a specific failure instead
// of silently succeeding (an admin UI must be able to say so).
BOOST_FIXTURE_TEST_CASE(AuthBranches_DisableUnknownUserFails, SessionBranchFixture)
{
	std::string error;

	BOOST_CHECK(!Service->DisableUser("no-such-user-id", error));
	BOOST_CHECK(!error.empty());
}

// A live refresh session whose account has been REMOVED underneath it must not
// mint a new access token: the session is revoked and the refresh fails.
BOOST_FIXTURE_TEST_CASE(AuthBranches_RefreshAfterAccountRemovedFails, SessionBranchFixture)
{
	const auto login = Login("bob", "bob-password-1");
	BOOST_REQUIRE(login.Success);
	BOOST_REQUIRE(!login.RefreshToken.empty());

	// The account goes away while the session lives on (bob is not the last
	// admin, so removal is permitted).
	std::string error;
	BOOST_REQUIRE_MESSAGE(Users->Remove(BobId, Groups->Registry(), error), error);

	const auto refreshed = Service->Refresh(login.RefreshToken, "10.0.0.9");

	BOOST_CHECK(!refreshed.Success);
	BOOST_CHECK(!refreshed.Error.empty());
	BOOST_CHECK(refreshed.AccessToken.empty());

	// The session was torn down, so the token cannot be retried.
	const auto retried = Service->Refresh(login.RefreshToken, "10.0.0.9");
	BOOST_CHECK(!retried.Success);
}

// Refreshing with a token that was never issued fails without touching any
// account.
BOOST_FIXTURE_TEST_CASE(AuthBranches_RefreshUnknownTokenFails, SessionBranchFixture)
{
	const auto refreshed = Service->Refresh("art_never-issued", "10.0.0.9");

	BOOST_CHECK(!refreshed.Success);
	BOOST_CHECK(!refreshed.Error.empty());
	BOOST_CHECK(refreshed.AccessToken.empty());
	BOOST_CHECK(refreshed.RefreshToken.empty());
}

// MintAccessToken re-reads the store so a tokver bump between the caller's
// snapshot and the mint is reflected; for a record that is NOT in the store it
// falls back to the supplied snapshot rather than failing.
BOOST_FIXTURE_TEST_CASE(AuthBranches_MintAccessTokenFallsBackToSuppliedRecord, SessionBranchFixture)
{
	Auth::UserRecord detached;
	detached.Id = "detached-user-id";
	detached.Username = "detached";
	detached.TokenVersion = 7;

	const auto token = Service->MintAccessToken(detached);
	BOOST_REQUIRE(!token.empty());

	const auto claims = Codec->Verify(token);
	BOOST_REQUIRE(claims.has_value());
	BOOST_CHECK_EQUAL("detached-user-id", claims->Subject);
	BOOST_CHECK_EQUAL(7u, claims->TokenVersion);

	// ...whereas a record that IS in the store is re-read, so a tokver bump
	// after the snapshot is picked up.
	auto bob = Users->FindById(BobId);
	BOOST_REQUIRE(bob.has_value());
	const auto snapshot = *bob;

	Users->BumpTokenVersion(BobId);

	const auto fresh = Codec->Verify(Service->MintAccessToken(snapshot));
	BOOST_REQUIRE(fresh.has_value());
	BOOST_CHECK_EQUAL(Users->FindById(BobId)->TokenVersion, fresh->TokenVersion);
	BOOST_CHECK(fresh->TokenVersion != snapshot.TokenVersion);
}

BOOST_AUTO_TEST_SUITE_END()
