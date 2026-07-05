#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "auth/api_key_store.h"
#include "auth/group_store.h"
#include "auth/user_store.h"

using namespace AqualinkAutomate;

namespace
{
	namespace fs = std::filesystem;

	struct TempDirFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-auth-stores-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	Auth::UserRecord MakeUser(std::string username, std::vector<std::string> groups = {}, std::vector<std::string> direct = {})
	{
		Auth::UserRecord user;
		user.Username = std::move(username);
		user.PasswordHash = "$argon2id$fakehashforstoretests";
		user.Groups = std::move(groups);
		user.DirectEntitlements = Auth::EntitlementSet::Parse(direct);
		return user;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_AuthStores)

//-----------------------------------------------------------------------------
// USER STORE
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_UserStore_CreateFindPersistReload, TempDirFixture)
{
	const auto file = Dir / "users.json";
	std::string error;

	{
		auto store = Auth::UserStore::Load(file);
		BOOST_CHECK(store.Empty());

		BOOST_REQUIRE_MESSAGE(store.Create(MakeUser("Alice", { "Administrators" }), error), error);
		BOOST_REQUIRE_MESSAGE(store.Create(MakeUser("bob"), error), error);
	}

	auto reloaded = Auth::UserStore::Load(file);
	BOOST_CHECK_EQUAL(reloaded.Size(), 2u);

	// Case-insensitive lookup; record round-trips intact.
	const auto alice = reloaded.FindByUsername("alice");
	BOOST_REQUIRE(alice.has_value());
	BOOST_CHECK(!alice->Id.empty());
	BOOST_CHECK_EQUAL(alice->Groups.size(), 1u);
	BOOST_CHECK_EQUAL(alice->TokenVersion, 1u);
}

BOOST_FIXTURE_TEST_CASE(Test_UserStore_DuplicateUsernameRejected, TempDirFixture)
{
	auto store = Auth::UserStore::Load(Dir / "users.json");
	std::string error;

	BOOST_REQUIRE(store.Create(MakeUser("alice"), error));
	BOOST_CHECK(!store.Create(MakeUser("ALICE"), error));  // Case-insensitive clash.
}

BOOST_FIXTURE_TEST_CASE(Test_UserStore_BumpTokenVersionPersists, TempDirFixture)
{
	const auto file = Dir / "users.json";
	std::string error, id;

	{
		auto store = Auth::UserStore::Load(file);
		BOOST_REQUIRE(store.Create(MakeUser("alice"), error));
		id = store.FindByUsername("alice")->Id;

		BOOST_CHECK_EQUAL(store.BumpTokenVersion(id), 2u);
		BOOST_CHECK_EQUAL(store.BumpTokenVersion(id), 3u);
		BOOST_CHECK_EQUAL(store.BumpTokenVersion("no-such-id"), 0u);
	}

	BOOST_CHECK_EQUAL(Auth::UserStore::Load(file).FindById(id)->TokenVersion, 3u);
}

BOOST_FIXTURE_TEST_CASE(Test_UserStore_LastAdminProtection, TempDirFixture)
{
	const auto registry = Auth::GroupRegistry::WithBuiltIns();
	auto store = Auth::UserStore::Load(Dir / "users.json");
	std::string error;

	// One admin (via group) + one via DIRECT grant + one normal user.
	BOOST_REQUIRE(store.Create(MakeUser("admin1", { "Administrators" }), error));
	BOOST_REQUIRE(store.Create(MakeUser("admin2", {}, { "system.admin" }), error));
	BOOST_REQUIRE(store.Create(MakeUser("norm"), error));

	const auto admin1 = *store.FindByUsername("admin1");
	const auto admin2 = *store.FindByUsername("admin2");

	// Removing ONE admin is fine (another remains)...
	BOOST_CHECK_MESSAGE(store.Remove(admin1.Id, registry, error), error);

	// ...but the LAST admin can be neither removed, nor disabled, nor de-admined.
	BOOST_CHECK(!store.Remove(admin2.Id, registry, error));

	auto disabled = admin2;
	disabled.Disabled = true;
	BOOST_CHECK(!store.Update(disabled, registry, error));

	auto de_admined = admin2;
	de_admined.DirectEntitlements = {};
	BOOST_CHECK(!store.Update(de_admined, registry, error));

	// A non-admin change on the last admin still works (e.g. rename).
	auto renamed = admin2;
	renamed.Username = "admin2-renamed";
	BOOST_CHECK_MESSAGE(store.Update(renamed, registry, error), error);

	// And the normal user remains freely mutable.
	BOOST_CHECK_MESSAGE(store.Remove(store.FindByUsername("norm")->Id, registry, error), error);
}

BOOST_FIXTURE_TEST_CASE(Test_UserStore_CorruptFileThrows, TempDirFixture)
{
	const auto file = Dir / "users.json";

	{
		std::ofstream stream(file);
		stream << "not json";
	}

	BOOST_CHECK_THROW(Auth::UserStore::Load(file), std::runtime_error);
}

//-----------------------------------------------------------------------------
// GROUP STORE
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_GroupStore_SeedsBuiltInsAndPersistsGuestScope, TempDirFixture)
{
	const auto file = Dir / "groups.json";
	std::string error;

	{
		auto store = Auth::GroupStore::Load(file);

		// Built-ins exist from first run.
		BOOST_CHECK(store.Registry().Find(Auth::BuiltInGroups::EVERYONE).has_value());
		BOOST_CHECK(store.Registry().Find(Auth::BuiltInGroups::GUEST).has_value());
		BOOST_CHECK(store.Registry().Find(Auth::BuiltInGroups::ADMINISTRATORS)->Entitlements.Permits("system.admin"));

		// Admin scopes Guest (the guest-mode grant path).
		Auth::Group guest{ .Name = std::string{ Auth::BuiltInGroups::GUEST }, .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:AUX5" }) };
		BOOST_REQUIRE_MESSAGE(store.Upsert(std::move(guest), error), error);
	}

	// Scoping survives restart; built-in flag reasserted.
	auto reloaded = Auth::GroupStore::Load(file);
	const auto guest = reloaded.Registry().Find(Auth::BuiltInGroups::GUEST);

	BOOST_REQUIRE(guest.has_value());
	BOOST_CHECK(guest->BuiltIn);
	BOOST_CHECK(guest->Entitlements.Permits("equipment.control.aux", "AUX5"));
}

BOOST_FIXTURE_TEST_CASE(Test_GroupStore_BuiltInsUndeletable_CustomGroupsRemovable, TempDirFixture)
{
	auto store = Auth::GroupStore::Load(Dir / "groups.json");
	std::string error;

	BOOST_CHECK(!store.Remove(Auth::BuiltInGroups::GUEST, error));
	BOOST_CHECK(!store.Remove(Auth::BuiltInGroups::ADMINISTRATORS, error));

	BOOST_REQUIRE(store.Upsert(Auth::Group{ .Name = "Household", .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" }) }, error));
	BOOST_CHECK_MESSAGE(store.Remove("Household", error), error);
	BOOST_CHECK(!store.Registry().Find("Household").has_value());
}

//-----------------------------------------------------------------------------
// GROUP REGISTRY — EFFECTIVE ENTITLEMENT RESOLUTION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_GroupRegistry_ResolveMergesDirectEveryoneAndMemberships)
{
	auto registry = Auth::GroupRegistry::WithBuiltIns();

	// Everyone applies to every subject; a custom group adds more on top.
	auto everyone = *registry.Find(Auth::BuiltInGroups::EVERYONE);
	everyone.Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
	registry.Upsert(std::move(everyone));

	registry.Upsert(Auth::Group{ .Name = "Household", .Entitlements = Auth::EntitlementSet::Parse({ "equipment.control.aux:*" }) });

	const auto direct = Auth::EntitlementSet::Parse({ "schedules.view" });
	const auto effective = registry.ResolveEffectiveEntitlements(direct, { "Household" });

	BOOST_CHECK(effective.Permits("schedules.view"));                       // direct grant
	BOOST_CHECK(effective.Permits("equipment.view"));                       // Everyone grant
	BOOST_CHECK(effective.Permits("equipment.control.aux", "AUX3"));        // Household grant
	BOOST_CHECK(!effective.Permits("system.admin"));
}

BOOST_AUTO_TEST_CASE(Test_GroupRegistry_ResolveSkipsExplicitEveryoneMembershipAndUnknownGroups)
{
	auto registry = Auth::GroupRegistry::WithBuiltIns();

	auto everyone = *registry.Find(Auth::BuiltInGroups::EVERYONE);
	everyone.Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
	registry.Upsert(std::move(everyone));

	// "Everyone" listed as an explicit membership must be short-circuited (it is
	// already merged once, above) and a membership referencing a group that does
	// not exist must degrade to nothing rather than failing resolution.
	const auto effective = registry.ResolveEffectiveEntitlements({}, { std::string{ Auth::BuiltInGroups::EVERYONE }, "GhostGroup" });

	BOOST_CHECK(effective.Permits("equipment.view"));   // Everyone, applied exactly once.
	BOOST_CHECK_EQUAL(effective.Size(), 1u);            // Nothing double-counted or invented.
	BOOST_CHECK(!effective.Permits("system.admin"));
}

//-----------------------------------------------------------------------------
// API KEY STORE
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_ApiKeyStore_CreateAuthenticateRevoke, TempDirFixture)
{
	const auto file = Dir / "api-keys.json";
	std::string key_id, error;
	std::string secret;

	{
		auto store = Auth::ApiKeyStore::Load(file);
		secret = store.Create("home-assistant", Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:*" }), 0, key_id);

		BOOST_CHECK(secret.starts_with("aak_"));
	}

	// Secret is never persisted - only its digest.
	{
		std::ifstream stream(file);
		const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		BOOST_CHECK(contents.find(secret) == std::string::npos);
	}

	auto reloaded = Auth::ApiKeyStore::Load(file);

	const auto authenticated = reloaded.Authenticate(secret, 1000);
	BOOST_REQUIRE(authenticated.has_value());
	BOOST_CHECK_EQUAL(authenticated->Id, key_id);
	BOOST_CHECK(authenticated->Entitlements.Permits("equipment.control.aux", "AUX3"));
	BOOST_CHECK_EQUAL(reloaded.FindById(key_id)->LastUsedUnix, 1000);

	BOOST_CHECK(!reloaded.Authenticate("aak_wrong", 1000).has_value());

	BOOST_REQUIRE_MESSAGE(reloaded.Revoke(key_id, error), error);
	BOOST_CHECK(!reloaded.Authenticate(secret, 1001).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeyStore_ExpiryEnforced, TempDirFixture)
{
	auto store = Auth::ApiKeyStore::Load(Dir / "api-keys.json");
	std::string key_id;

	const auto secret = store.Create("short-lived", Auth::EntitlementSet::Parse({ "equipment.view" }), 5000, key_id);

	BOOST_CHECK(store.Authenticate(secret, 4999).has_value());
	BOOST_CHECK(!store.Authenticate(secret, 5000).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeyStore_EmptySecretRejected, TempDirFixture)
{
	auto store = Auth::ApiKeyStore::Load(Dir / "api-keys.json");
	std::string key_id;
	store.Create("some-key", Auth::EntitlementSet::Parse({ "equipment.view" }), 0, key_id);

	// An empty presented secret must be rejected outright (before any digest work),
	// never matching a stored key.
	BOOST_CHECK(!store.Authenticate("", 1000).has_value());
}

BOOST_FIXTURE_TEST_CASE(Test_AuthStore_UnknownSchemaVersionThrows, TempDirFixture)
{
	const auto file = Dir / "api-keys.json";

	{
		// A newer/unknown schema version must be fatal rather than silently
		// dropping fields the current build does not understand.
		std::ofstream stream(file);
		stream << R"({"schema_version":999999,"keys":[]})";
	}

	BOOST_CHECK_THROW(Auth::ApiKeyStore::Load(file), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeyStore_BootstrapLegacyTokenFoldIn, TempDirFixture)
{
	const auto file = Dir / "api-keys.json";

	{
		auto store = Auth::ApiKeyStore::Load(file);
		store.SeedBootstrapKey("my-legacy-operator-token");
		store.SeedBootstrapKey("my-legacy-operator-token");  // Idempotent.

		BOOST_CHECK_EQUAL(store.All().size(), 1u);
	}

	auto reloaded = Auth::ApiKeyStore::Load(file);

	// The legacy shared token authenticates as a system.admin machine subject.
	const auto authenticated = reloaded.Authenticate("my-legacy-operator-token", 42);
	BOOST_REQUIRE(authenticated.has_value());
	BOOST_CHECK(authenticated->Entitlements.Permits("system.admin"));

	// A CHANGED legacy token reseeds (old value stops working).
	reloaded.SeedBootstrapKey("rotated-token");
	BOOST_CHECK(!reloaded.Authenticate("my-legacy-operator-token", 43).has_value());
	BOOST_CHECK(reloaded.Authenticate("rotated-token", 43).has_value());
	BOOST_CHECK_EQUAL(reloaded.All().size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
