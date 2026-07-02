#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "preferences/user_preferences_store.h"

using namespace AqualinkAutomate;

namespace
{
	namespace fs = std::filesystem;

	struct TempDirFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-userprefs-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	nlohmann::json Defaults()
	{
		return {
			{ "temperature_units", "Celsius" },
			{ "theme", "system" },
			{ "accent", "teal" },
			{ "chemistry_bands", nlohmann::json::object() }
		};
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_UserPreferencesStore)

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_DefaultsWhenNoOverrides, TempDirFixture)
{
	auto store = Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json");
	store.SetDefaults(Defaults());

	const auto effective = store.Effective("user-1");

	BOOST_CHECK_EQUAL(effective.value("temperature_units", ""), "Celsius");
	BOOST_CHECK_EQUAL(effective.value("theme", ""), "system");
	BOOST_CHECK_EQUAL(effective.value("accent", ""), "teal");
	BOOST_CHECK(!store.HasOverrides("user-1"));
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_OverridesLayerOverDefaultsAndPersist, TempDirFixture)
{
	const auto file = Dir / "user_preferences.json";
	std::string error;

	{
		auto store = Preferences::UserPreferencesStore::Load(file);
		store.SetDefaults(Defaults());

		BOOST_REQUIRE_MESSAGE(store.Apply("user-1", { { "theme", "dark" }, { "temperature_units", "Fahrenheit" } }, error), error);
	}

	// Reload: overrides persisted; unset fields still fall back to defaults.
	auto reloaded = Preferences::UserPreferencesStore::Load(file);
	reloaded.SetDefaults(Defaults());

	const auto effective = reloaded.Effective("user-1");
	BOOST_CHECK_EQUAL(effective.value("theme", ""), "dark");            // overridden
	BOOST_CHECK_EQUAL(effective.value("temperature_units", ""), "Fahrenheit");  // overridden
	BOOST_CHECK_EQUAL(effective.value("accent", ""), "teal");           // default
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_IsolatedBetweenUsers, TempDirFixture)
{
	auto store = Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json");
	store.SetDefaults(Defaults());

	std::string error;
	BOOST_REQUIRE(store.Apply("alice", { { "theme", "dark" } }, error));
	BOOST_REQUIRE(store.Apply("bob", { { "theme", "light" } }, error));

	BOOST_CHECK_EQUAL(store.Effective("alice").value("theme", ""), "dark");
	BOOST_CHECK_EQUAL(store.Effective("bob").value("theme", ""), "light");
	BOOST_CHECK_EQUAL(store.Effective("carol").value("theme", ""), "system");  // untouched -> default
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_RejectsInvalidFields, TempDirFixture)
{
	auto store = Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json");
	std::string error;

	BOOST_CHECK(!store.Apply("user-1", { { "temperature_units", "Kelvin" } }, error));
	BOOST_CHECK(!store.Apply("user-1", { { "theme", "neon" } }, error));
	BOOST_CHECK(!store.Apply("user-1", { { "accent", "" } }, error));
	BOOST_CHECK(!store.Apply("user-1", { { "chemistry_bands", "not-an-object" } }, error));

	// A rejected apply stores nothing.
	BOOST_CHECK(!store.HasOverrides("user-1"));
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_SystemFieldsCannotLeakIn, TempDirFixture)
{
	auto store = Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json");
	store.SetDefaults(Defaults());

	std::string error;

	// A full-prefs document (as the frontend might PUT) carrying system/admin
	// fields is accepted but only the per-user keys are retained.
	BOOST_REQUIRE(store.Apply("user-1", {
		{ "theme", "dark" },
		{ "label_overrides", { { "aux1", "Pool Light" } } },
		{ "history", { { "retention_days", 1 } } },
		{ "spa_switch_buttons", nlohmann::json::object() }
	}, error));

	const auto effective = store.Effective("user-1");
	BOOST_CHECK_EQUAL(effective.value("theme", ""), "dark");
	BOOST_CHECK(!effective.contains("label_overrides"));
	BOOST_CHECK(!effective.contains("history"));
	BOOST_CHECK(!effective.contains("spa_switch_buttons"));
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_ForgetDropsOverrides, TempDirFixture)
{
	auto store = Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json");
	store.SetDefaults(Defaults());

	std::string error;
	BOOST_REQUIRE(store.Apply("user-1", { { "theme", "dark" } }, error));
	BOOST_CHECK(store.HasOverrides("user-1"));

	store.Forget("user-1");
	BOOST_CHECK(!store.HasOverrides("user-1"));
	BOOST_CHECK_EQUAL(store.Effective("user-1").value("theme", ""), "system");  // back to default
}

BOOST_FIXTURE_TEST_CASE(Test_UserPrefs_CorruptFileThrows, TempDirFixture)
{
	const auto file = Dir / "user_preferences.json";

	{
		std::ofstream out(file);
		out << "not json";
	}

	BOOST_CHECK_THROW(Preferences::UserPreferencesStore::Load(file), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
