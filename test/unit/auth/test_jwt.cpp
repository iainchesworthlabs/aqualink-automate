#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "exceptions/exception_auth_storeerror.h"

using namespace AqualinkAutomate;
using namespace std::chrono_literals;

namespace
{
	namespace fs = std::filesystem;

	// Unique scratch directory per test-case; removed on scope exit.
	struct TempDirFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-auth-jwt-test-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	// A controllable clock so expiry/leeway tests are deterministic.
	struct FakeClock
	{
		std::chrono::system_clock::time_point Now{ std::chrono::system_clock::now() };

		Auth::JwtCodec::NowFn Fn()
		{
			return [this]() { return Now; };
		}
	};

	Auth::TokenClaims MakeClaims()
	{
		Auth::TokenClaims claims;
		claims.Subject = "user-42";
		claims.Provider = Auth::SubjectProvider::Local;
		claims.TokenVersion = 7;
		claims.Groups = { "Household" };
		claims.Entitlements = { "equipment.control.aux:AUX3", "equipment.view" };
		return claims;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_Jwt)

//-----------------------------------------------------------------------------
// KEY STORE
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_CreatesAndReloads, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	const auto created = Auth::JwtKeyStore::LoadOrCreate(key_file);

	BOOST_CHECK(fs::exists(key_file));
	BOOST_CHECK_EQUAL(created.KeyCount(), 1u);
	BOOST_CHECK(!created.Active().Kid.empty());
	BOOST_CHECK_EQUAL(created.Active().Secret.size(), 32u);

	// Reload picks up the SAME key (sessions survive restart).
	const auto reloaded = Auth::JwtKeyStore::LoadOrCreate(key_file);

	BOOST_CHECK_EQUAL(reloaded.Active().Kid, created.Active().Kid);
	BOOST_CHECK(reloaded.Active().Secret == created.Active().Secret);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_RotateKeepsGraceKey, TempDirFixture)
{
	auto store = Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key");

	const auto original_kid = store.Active().Kid;

	store.Rotate();

	BOOST_CHECK_EQUAL(store.KeyCount(), 2u);
	BOOST_CHECK(store.Active().Kid != original_kid);
	BOOST_CHECK(store.Find(original_kid).has_value()); // Grace key retained.

	store.Rotate();

	BOOST_CHECK_EQUAL(store.KeyCount(), 2u);              // Bounded retention.
	BOOST_CHECK(!store.Find(original_kid).has_value());  // Oldest dropped.
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_MalformedFileThrowsRatherThanRegenerating, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		std::ofstream file(key_file);
		file << "not json at all";
	}

	// Silent regeneration would invalidate every outstanding session; the
	// store must surface the problem instead.
	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_OddLengthHexSecretThrows, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		// secret_hex has an odd number of characters -> FromHex rejects it and the
		// entry is treated as malformed rather than silently accepted.
		std::ofstream file(key_file);
		file << R"({"schema_version":1,"active":"deadbeef","keys":[{"kid":"deadbeef","secret_hex":"abc","created":1}]})";
	}

	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_NonHexSecretThrows, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		// Even length but a non-hex digit -> FromHex's nibble decode fails.
		std::ofstream file(key_file);
		file << R"({"schema_version":1,"active":"deadbeef","keys":[{"kid":"deadbeef","secret_hex":"zz","created":1}]})";
	}

	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_EmptyKidThrows, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		// Well-formed hex but an empty kid is still a malformed entry.
		std::ofstream file(key_file);
		file << R"({"schema_version":1,"active":"","keys":[{"kid":"","secret_hex":"aabb","created":1}]})";
	}

	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_EmptyKeysArrayThrows, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		// Parseable JSON, no keys -> refuse rather than silently regenerate.
		std::ofstream file(key_file);
		file << R"({"schema_version":1,"active":"","keys":[]})";
	}

	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_UppercaseHexSecretLoads, TempDirFixture)
{
	const auto key_file = Dir / "jwt-signing.key";

	{
		// FromHex accepts upper-case hex digits: a hand-written key file with an
		// upper-case secret must load cleanly (32 bytes = 64 hex chars).
		std::ofstream file(key_file);
		file << R"({"schema_version":1,"active":"ABCDEF0123456789","keys":[{"kid":"ABCDEF0123456789","secret_hex":"AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899","created":123}]})";
	}

	const auto store = Auth::JwtKeyStore::LoadOrCreate(key_file);

	BOOST_CHECK_EQUAL(store.KeyCount(), 1u);
	BOOST_CHECK_EQUAL(store.Active().Kid, "ABCDEF0123456789");
	BOOST_CHECK_EQUAL(store.Active().Secret.size(), 32u);
}

BOOST_FIXTURE_TEST_CASE(Test_JwtKeyStore_UnwritableKeyFileThrows, TempDirFixture)
{
	// Target a key file inside a directory that does NOT exist: the atomic
	// write-temp-then-rename cannot even open the ".tmp" stream, so LoadOrCreate
	// (which Saves the freshly generated key) must surface the failure instead of
	// pretending a key was persisted.
	const auto key_file = Dir / "missing-subdir" / "jwt-signing.key";

	BOOST_CHECK(!fs::exists(key_file.parent_path()));
	BOOST_CHECK_THROW(Auth::JwtKeyStore::LoadOrCreate(key_file), AqualinkAutomate::Exceptions::Auth_StoreError);
}

//-----------------------------------------------------------------------------
// CODEC — ROUND TRIP + CLAIMS
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_SignVerifyRoundTrip, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	FakeClock clock;
	const Auth::JwtCodec codec(store, Auth::JwtCodec::Config{ .Now = clock.Fn() });

	const auto token = codec.Sign(MakeClaims());

	std::string error;
	const auto verified = codec.Verify(token, &error);

	BOOST_REQUIRE_MESSAGE(verified.has_value(), error);
	BOOST_CHECK_EQUAL(verified->Subject, "user-42");
	BOOST_CHECK(Auth::SubjectProvider::Local == verified->Provider);
	BOOST_CHECK_EQUAL(verified->TokenVersion, 7u);
	BOOST_REQUIRE_EQUAL(verified->Groups.size(), 1u);
	BOOST_CHECK_EQUAL(verified->Groups[0], "Household");

	// The entitlement claim is self-describing and directly assertable.
	BOOST_CHECK(verified->EntitlementsInToken);
	BOOST_REQUIRE_EQUAL(verified->Entitlements.size(), 2u);
	BOOST_CHECK_EQUAL(verified->Entitlements[0], "equipment.control.aux:AUX3");
	BOOST_CHECK_EQUAL(verified->Entitlements[1], "equipment.view");
}

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_RejectsTamperedToken, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	const Auth::JwtCodec codec(store, {});

	auto token = codec.Sign(MakeClaims());

	// Flip the FIRST character of the signature segment.  (Not the last: the
	// final base64url character carries slack bits a lenient decoder ignores,
	// so flipping it may decode to the SAME signature bytes and still verify.)
	const auto signature_start = token.rfind('.') + 1;
	token[signature_start] = ('A' == token[signature_start]) ? 'B' : 'A';

	BOOST_CHECK(!codec.Verify(token).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_RejectsWrongIssuerAndAudience, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	const Auth::JwtCodec minter(store, Auth::JwtCodec::Config{ .Issuer = "some-other-app" });
	const Auth::JwtCodec verifier(store, {});

	BOOST_CHECK(!verifier.Verify(minter.Sign(MakeClaims())).has_value());
}

//-----------------------------------------------------------------------------
// CODEC — EXPIRY, LEEWAY (clock skew), ROTATION
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_ExpiryHonoursLeeway, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	FakeClock clock;
	const Auth::JwtCodec codec(store, Auth::JwtCodec::Config{ .AccessTokenTtl = 15min, .LeewaySeconds = 60s, .Now = clock.Fn() });

	const auto token = codec.Sign(MakeClaims());

	// Just before expiry: fine.
	clock.Now += 14min;
	BOOST_CHECK(codec.Verify(token).has_value());

	// Past expiry but within leeway (RTC-less Pi clock skew): still fine.
	clock.Now += 1min + 30s;
	BOOST_CHECK(codec.Verify(token).has_value());

	// Past expiry AND leeway: rejected.
	clock.Now += 2min;
	BOOST_CHECK(!codec.Verify(token).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_RotationGrace, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	const Auth::JwtCodec codec(store, {});

	const auto pre_rotation_token = codec.Sign(MakeClaims());

	store->Rotate();

	// Token minted before rotation still verifies via the grace key (kid).
	BOOST_CHECK(codec.Verify(pre_rotation_token).has_value());

	// A second rotation drops that key; the old token dies with it.
	store->Rotate();
	BOOST_CHECK(!codec.Verify(pre_rotation_token).has_value());

	// Tokens minted with the current key are unaffected.
	BOOST_CHECK(codec.Verify(codec.Sign(MakeClaims())).has_value());
}

//-----------------------------------------------------------------------------
// CODEC — ENTITLEMENT-CLAIM SIZE OVERFLOW
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_EntitlementOverflowElidesClaim, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	// A tiny budget forces the overflow path without needing hundreds of grants.
	const Auth::JwtCodec codec(store, Auth::JwtCodec::Config{ .EntClaimBudgetBytes = 64 });

	auto claims = MakeClaims();

	for (int i = 0; i < 32; ++i)
	{
		claims.Entitlements.push_back(std::format("equipment.control.aux:5e17c9b2-0001-4a2b-9c1d-70e6a1b2c3{:02x}", i));
	}

	const auto verified = codec.Verify(codec.Sign(claims));

	BOOST_REQUIRE(verified.has_value());
	BOOST_CHECK(!verified->EntitlementsInToken);       // ent elided...
	BOOST_CHECK(verified->Entitlements.empty());       // ...nothing smuggled...
	BOOST_REQUIRE_EQUAL(verified->Groups.size(), 1u);  // ...groups still there
	BOOST_CHECK_EQUAL(verified->Groups[0], "Household"); //    for re-resolution.
}

BOOST_FIXTURE_TEST_CASE(Test_JwtCodec_VerifyFailurePopulatesErrorString, TempDirFixture)
{
	auto store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt-signing.key"));

	const Auth::JwtCodec codec(store, {});

	auto token = codec.Sign(MakeClaims());

	// Tamper with the signature segment so verification fails, and pass a non-null
	// error pointer: the failure reason must be written back to the caller.
	const auto signature_start = token.rfind('.') + 1;
	token[signature_start] = ('A' == token[signature_start]) ? 'B' : 'A';

	std::string error;
	const auto verified = codec.Verify(token, &error);

	BOOST_CHECK(!verified.has_value());
	BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()
