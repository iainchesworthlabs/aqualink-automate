#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/kiosk_service.h"
#include "auth/kiosk_store.h"
#include "auth/password_hasher.h"
#include "auth/session_store.h"
#include "auth/subject_resolver.h"
#include "http/server/server_types.h"
#include "utility/offload_pool.h"

using namespace AqualinkAutomate;
using namespace std::chrono_literals;

namespace
{
	namespace fs = std::filesystem;

	// Full kiosk fixture: real stores + codec in a temp dir, controllable
	// clock, fast argon2 params, a scoped Guest group and a "Household" target
	// group, and a subject resolver wired with the same kiosk store.
	struct KioskFixture
	{
		KioskFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-kiosk-test-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);

			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(Dir / "sessions.json"));
			Kiosk = std::make_shared<Auth::KioskStore>(Auth::KioskStore::Load(Dir / "kiosk.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{ .Now = [this]() { return Now; } });

			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{ .JsonlFile = Dir / "audit.jsonl" });

			// Guest sees equipment; the kiosk "Household" group adds aux control.
			std::string error;
			Auth::Group guest{ .Name = std::string{ Auth::BuiltInGroups::GUEST }, .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" }) };
			BOOST_REQUIRE(Groups->Upsert(std::move(guest), error));
			Auth::Group household{ .Name = "Household", .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:*" }) };
			BOOST_REQUIRE(Groups->Upsert(std::move(household), error));

			Auth::KioskService::Config config;
			config.HashParams = Auth::PasswordHasher::TestParams();
			config.MinPinLength = 4;
			config.MaxFailures = 3;
			config.Now = [this]() { return Now; };

			Service = std::make_unique<Auth::KioskService>(Kiosk, Groups, Sessions, Codec, Offload, *Audit, std::move(config));

			Resolver = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = Groups->SharedRegistry(),
				.Codec = Codec,
				.Kiosk = Kiosk });
		}

		~KioskFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		Auth::KioskService::SetPinResult SetPin(const std::string& pin, const std::string& group)
		{
			boost::asio::io_context io;
			auto guard = boost::asio::make_work_guard(io);

			Auth::KioskService::SetPinResult captured;
			Service->SetPin(pin, group, "admin-id", "10.0.0.1", io.get_executor(),
				[&](Auth::KioskService::SetPinResult r) { captured = std::move(r); guard.reset(); });
			io.run();
			return captured;
		}

		Auth::KioskService::LoginResult LoginWithPin(const std::string& pin)
		{
			boost::asio::io_context io;
			auto guard = boost::asio::make_work_guard(io);

			Auth::KioskService::LoginResult captured;
			Service->LoginWithPin(pin, "10.0.0.2", "kiosk-agent", io.get_executor(),
				[&](Auth::KioskService::LoginResult r) { captured = std::move(r); guard.reset(); });
			io.run();
			return captured;
		}

		Auth::Subject Resolve(std::string_view bearer = {})
		{
			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::get);
			req.target("/api/equipment");

			if (!bearer.empty())
			{
				req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ bearer });
			}

			return Resolver(req, false);
		}

		std::chrono::system_clock::time_point Now{ std::chrono::system_clock::now() };

		fs::path Dir;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::SessionStore> Sessions;
		std::shared_ptr<Auth::KioskStore> Kiosk;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::unique_ptr<Auth::AuditLog> Audit;
		Utility::OffloadPool Offload{ 1 };
		std::unique_ptr<Auth::KioskService> Service;
		HTTP::Routing::SubjectResolver Resolver;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_Kiosk)

//-----------------------------------------------------------------------------
// KIOSK STORE (persistence + tokver)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_KioskStore_ConfigurePersistsAndBumpsTokver, KioskFixture)
{
	const auto file = Dir / "kiosk-standalone.json";

	std::uint32_t first_tokver{ 0 };
	{
		auto store = Auth::KioskStore::Load(file);
		BOOST_CHECK(!store.Enabled());
		const auto before = store.TokenVersion();
		store.Configure("hash-value", "Household");
		BOOST_CHECK(store.Enabled());
		BOOST_CHECK_EQUAL(store.TargetGroup(), "Household");
		BOOST_CHECK(store.TokenVersion() > before);   // A change must bump tokver.
		first_tokver = store.TokenVersion();
	}

	// Reload: the configuration survives a restart.
	auto reloaded = Auth::KioskStore::Load(file);
	BOOST_CHECK(reloaded.Enabled());
	BOOST_CHECK_EQUAL(reloaded.TargetGroup(), "Household");
	BOOST_CHECK_EQUAL(reloaded.PinHash(), "hash-value");
	BOOST_CHECK_EQUAL(reloaded.TokenVersion(), first_tokver);
}

BOOST_FIXTURE_TEST_CASE(Test_KioskStore_DisableClearsAndBumps, KioskFixture)
{
	auto store = Auth::KioskStore::Load(Dir / "kiosk-disable.json");
	store.Configure("hash-value", "Household");
	const auto enabled_tokver = store.TokenVersion();

	store.Disable();
	BOOST_CHECK(!store.Enabled());
	BOOST_CHECK(store.PinHash().empty());
	BOOST_CHECK(store.TargetGroup().empty());
	BOOST_CHECK(store.TokenVersion() > enabled_tokver);
}

//-----------------------------------------------------------------------------
// KIOSK SERVICE (set PIN, login, lockout)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_KioskService_SetPinRejectsShortPin, KioskFixture)
{
	const auto result = SetPin("12", "Household");
	BOOST_CHECK(!result.Success);
	BOOST_CHECK(!Kiosk->Enabled());
}

BOOST_FIXTURE_TEST_CASE(Test_KioskService_SetPinRejectsUnknownGroup, KioskFixture)
{
	const auto result = SetPin("1234", "NoSuchGroup");
	BOOST_CHECK(!result.Success);
	BOOST_CHECK(!Kiosk->Enabled());
}

BOOST_FIXTURE_TEST_CASE(Test_KioskService_PinLoginMintsTargetGroupSession, KioskFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);
	BOOST_REQUIRE(Kiosk->Enabled());

	const auto result = LoginWithPin("2468");
	BOOST_REQUIRE(result.Success);
	BOOST_CHECK(!result.AccessToken.empty());
	BOOST_CHECK(result.RefreshToken.starts_with("art_"));

	// The session appears under the kiosk subject id (revocable / listable).
	BOOST_CHECK_EQUAL(Sessions->ForUser(std::string{ Auth::KioskService::SubjectId }).size(), 1u);

	// The minted token resolves to the Household scope (aux control), NOT admin.
	const auto subject = Resolve(result.AccessToken);
	BOOST_CHECK(subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::KioskPin == subject.Provider);
	BOOST_CHECK(subject.Entitlements.Permits("equipment.view"));
	BOOST_CHECK(subject.Entitlements.Permits("equipment.control.aux", "AUX3"));
	BOOST_CHECK(!subject.Entitlements.Permits("system.admin"));
	// A shared terminal has no "self" preferences grant.
	BOOST_CHECK(!subject.Entitlements.Permits("prefs.self"));
}

BOOST_FIXTURE_TEST_CASE(Test_KioskService_WrongPinFails, KioskFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);

	const auto result = LoginWithPin("0000");
	BOOST_CHECK(!result.Success);
	BOOST_CHECK(!result.LockedOut);
	BOOST_CHECK(result.AccessToken.empty());
}

BOOST_FIXTURE_TEST_CASE(Test_KioskService_LoginFailsWhenDisabled, KioskFixture)
{
	// No PIN configured -> even a plausible PIN fails (against the decoy hash).
	const auto result = LoginWithPin("2468");
	BOOST_CHECK(!result.Success);
}

BOOST_FIXTURE_TEST_CASE(Test_KioskService_LockoutAfterMaxFailures, KioskFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);

	// MaxFailures == 3: the third wrong attempt trips the lockout.
	BOOST_CHECK(!LoginWithPin("0000").LockedOut);
	BOOST_CHECK(!LoginWithPin("0000").LockedOut);
	LoginWithPin("0000");

	// The next attempt — even with the CORRECT PIN — is locked out.
	const auto locked = LoginWithPin("2468");
	BOOST_CHECK(locked.LockedOut);
	BOOST_CHECK(!locked.Success);

	// After the lockout window elapses, the correct PIN works again.
	Now += 16min;
	const auto ok = LoginWithPin("2468");
	BOOST_CHECK(ok.Success);
}

//-----------------------------------------------------------------------------
// SUBJECT RESOLVER (kiosk-token validation + revocation)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_Resolver_KioskTokenRejectedAfterDisable, KioskFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);
	const auto token = LoginWithPin("2468").AccessToken;
	BOOST_REQUIRE(!token.empty());

	BOOST_CHECK(Resolve(token).Authenticated);

	// Disabling the kiosk bumps tokver -> the outstanding access token is stale
	// and the request degrades to the anonymous Guest scope.
	Kiosk->Disable();

	const auto subject = Resolve(token);
	BOOST_CHECK(!subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::Anonymous == subject.Provider);
	// Guest still sees equipment (its configured scope) but not aux control.
	BOOST_CHECK(subject.Entitlements.Permits("equipment.view"));
	BOOST_CHECK(!subject.Entitlements.Permits("equipment.control.aux", "AUX3"));
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_KioskTokenRejectedWithoutKioskDep, KioskFixture)
{
	BOOST_REQUIRE(SetPin("2468", "Household").Success);
	const auto token = LoginWithPin("2468").AccessToken;

	// A resolver with no kiosk store wired must reject kiosk tokens outright.
	auto resolver = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
		.Groups = Groups->SharedRegistry(),
		.Codec = Codec });

	HTTP::Request req;
	req.version(11);
	req.method(boost::beast::http::verb::get);
	req.target("/api/equipment");
	req.set(boost::beast::http::field::authorization, "Bearer " + token);

	const auto subject = resolver(req, false);
	BOOST_CHECK(!subject.Authenticated);
}

BOOST_AUTO_TEST_SUITE_END()
