#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "auth/audit_log.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/password_hasher.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/user_store.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;
using namespace std::chrono_literals;

namespace
{
	namespace fs = std::filesystem;

	struct TempDirFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-session-test-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	// Full service fixture: real stores + codec in a temp dir, controllable
	// clock, fast argon2 params, one seeded user.
	struct ServiceFixture : TempDirFixture
	{
		ServiceFixture()
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

			// Seed one admin user with a real (fast-params) hash.
			std::string error;
			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = Auth::PasswordHasher::Hash("correct-horse-battery", Auth::PasswordHasher::TestParams());
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);

			AliceId = Users->FindByUsername("alice")->Id;
		}

		// Drive an async login to completion on this thread's io_context.
		Auth::SessionService::LoginResult Login(const std::string& username, const std::string& password)
		{
			boost::asio::io_context io_context;
			auto guard = boost::asio::make_work_guard(io_context);

			Auth::SessionService::LoginResult captured;

			Service->Login(username, password, "192.168.1.50", "test-agent", io_context.get_executor(),
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
		std::string AliceId;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_Session)

//-----------------------------------------------------------------------------
// SESSION STORE (refresh-token mechanics)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_SessionStore_CreateRotateSingleUse, TempDirFixture)
{
	auto store = Auth::SessionStore::Load(Dir / "sessions.json");

	std::string session_id;
	const auto original = store.Create("user-1", 10'000, "ua", "ip", 1'000, session_id);

	BOOST_CHECK(original.starts_with("art_"));

	// Rotation succeeds once...
	const auto first = store.Rotate(original, 2'000);
	BOOST_REQUIRE(first.Success);
	BOOST_CHECK_EQUAL(first.UserId, "user-1");
	BOOST_CHECK(first.NewRefreshSecret != original);

	// ...and the ROTATED-OUT token replayed = reuse detected, session revoked.
	const auto replay = store.Rotate(original, 3'000);
	BOOST_CHECK(!replay.Success);
	BOOST_CHECK(replay.ReuseDetected);
	BOOST_CHECK_EQUAL(replay.UserId, "user-1");

	// The stolen session's CURRENT token is dead too.
	BOOST_CHECK(!store.Rotate(first.NewRefreshSecret, 3'500).Success);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionStore_ExpiryAndRevocationPersist, TempDirFixture)
{
	const auto file = Dir / "sessions.json";
	std::string expired_id, live_id;
	std::string expired_secret, live_secret;

	{
		auto store = Auth::SessionStore::Load(file);
		expired_secret = store.Create("user-1", 5'000, "ua", "ip", 1'000, expired_id);
		live_secret = store.Create("user-1", 50'000, "ua", "ip", 1'000, live_id);
	}

	auto reloaded = Auth::SessionStore::Load(file);
	BOOST_CHECK_EQUAL(reloaded.All().size(), 2u);

	// Expired session fails to rotate (and is pruned).
	BOOST_CHECK(!reloaded.Rotate(expired_secret, 6'000).Success);

	// Live session still rotates after the reload (digests persisted).
	BOOST_CHECK(reloaded.Rotate(live_secret, 6'000).Success);

	BOOST_CHECK_EQUAL(reloaded.RevokeAllForUser("user-1"), 1u);
	BOOST_CHECK(reloaded.All().empty());
}

//-----------------------------------------------------------------------------
// SESSION SERVICE — LOGIN
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_SessionService_LoginSuccessMintsEntitledToken, ServiceFixture)
{
	const auto result = Login("alice", "correct-horse-battery");

	BOOST_REQUIRE_MESSAGE(result.Success, result.Error);
	BOOST_CHECK(result.RefreshToken.starts_with("art_"));
	BOOST_CHECK(!result.SessionId.empty());

	// The access token is self-describing: subject, tokver, entitlements.
	const auto claims = Codec->Verify(result.AccessToken);
	BOOST_REQUIRE(claims.has_value());
	BOOST_CHECK_EQUAL(claims->Subject, AliceId);
	BOOST_CHECK_EQUAL(claims->TokenVersion, 1u);
	BOOST_REQUIRE(claims->EntitlementsInToken);
	BOOST_REQUIRE_EQUAL(claims->Entitlements.size(), 1u);
	BOOST_CHECK_EQUAL(claims->Entitlements[0], "system.admin");  // Administrators group.
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_WrongPasswordAndUnknownUserSameError, ServiceFixture)
{
	const auto wrong_password = Login("alice", "wrong-password");
	const auto unknown_user = Login("mallory", "whatever-password");

	BOOST_CHECK(!wrong_password.Success);
	BOOST_CHECK(!unknown_user.Success);

	// Indistinguishable responses: no account enumeration.
	BOOST_CHECK_EQUAL(wrong_password.Error, unknown_user.Error);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_DisabledUserCannotLogin, ServiceFixture)
{
	// A second admin so alice can be disabled past last-admin protection.
	std::string error;
	Auth::UserRecord bob;
	bob.Username = "bob";
	bob.PasswordHash = Auth::PasswordHasher::Hash("bobs-long-password", Auth::PasswordHasher::TestParams());
	bob.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
	BOOST_REQUIRE(Users->Create(std::move(bob), error));

	BOOST_REQUIRE_MESSAGE(Service->DisableUser(AliceId, error), error);

	BOOST_CHECK(!Login("alice", "correct-horse-battery").Success);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_LockoutAfterRepeatedFailures, ServiceFixture)
{
	for (int i = 0; i < 5; ++i)
	{
		BOOST_CHECK(!Login("alice", "wrong-password").Success);
	}

	// Locked: even the CORRECT password is refused during the window.
	const auto locked = Login("alice", "correct-horse-battery");
	BOOST_CHECK(!locked.Success);
	BOOST_CHECK(locked.LockedOut);

	// Window elapses -> login works again.
	Now += 16min;
	BOOST_CHECK(Login("alice", "correct-horse-battery").Success);
}

//-----------------------------------------------------------------------------
// SESSION SERVICE — REFRESH / LOGOUT / REVOCATION (D15)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_SessionService_RefreshRotatesAndReuseRevokes, ServiceFixture)
{
	const auto login = Login("alice", "correct-horse-battery");
	BOOST_REQUIRE(login.Success);

	const auto refreshed = Service->Refresh(login.RefreshToken, "ip");
	BOOST_REQUIRE_MESSAGE(refreshed.Success, refreshed.Error);
	BOOST_CHECK(Codec->Verify(refreshed.AccessToken).has_value());

	// Replaying the ORIGINAL refresh token kills the session entirely.
	BOOST_CHECK(!Service->Refresh(login.RefreshToken, "ip").Success);
	BOOST_CHECK(!Service->Refresh(refreshed.RefreshToken, "ip").Success);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_LogoutEndsSession, ServiceFixture)
{
	const auto login = Login("alice", "correct-horse-battery");
	BOOST_REQUIRE(login.Success);

	BOOST_CHECK(Service->Logout(login.RefreshToken, "ip"));
	BOOST_CHECK(!Service->Refresh(login.RefreshToken, "ip").Success);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_LogoutAllBumpsTokver, ServiceFixture)
{
	const auto login = Login("alice", "correct-horse-battery");
	BOOST_REQUIRE(login.Success);

	const auto before = Codec->Verify(login.AccessToken);
	BOOST_REQUIRE(before.has_value());

	BOOST_CHECK_EQUAL(Service->LogoutAll(AliceId, "ip"), 1u);

	// Refresh sessions are gone AND the store's tokver moved past the token's
	// claim — the subject resolver rejects it on the next request (D15).
	BOOST_CHECK(!Service->Refresh(login.RefreshToken, "ip").Success);
	BOOST_CHECK_GT(Users->FindById(AliceId)->TokenVersion, before->TokenVersion);
}

BOOST_FIXTURE_TEST_CASE(Test_SessionService_DisableRevokesEverythingButRespectsLastAdmin, ServiceFixture)
{
	const auto login = Login("alice", "correct-horse-battery");
	BOOST_REQUIRE(login.Success);

	// Alice is the ONLY admin: disabling her must be refused outright.
	std::string error;
	BOOST_CHECK(!Service->DisableUser(AliceId, error));
	BOOST_CHECK(!error.empty());

	// With a second admin present the disable goes through and revokes.
	Auth::UserRecord bob;
	bob.Username = "bob";
	bob.PasswordHash = Auth::PasswordHasher::Hash("bobs-long-password", Auth::PasswordHasher::TestParams());
	bob.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
	BOOST_REQUIRE(Users->Create(std::move(bob), error));

	BOOST_REQUIRE_MESSAGE(Service->DisableUser(AliceId, error), error);
	BOOST_CHECK(Users->FindById(AliceId)->Disabled);
	BOOST_CHECK(!Service->Refresh(login.RefreshToken, "ip").Success);
	BOOST_CHECK(Sessions->ForUser(AliceId).empty());
}

BOOST_AUTO_TEST_SUITE_END()
