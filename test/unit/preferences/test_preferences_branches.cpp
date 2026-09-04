#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "exceptions/exception_preferences_storeerror.h"
#include "kernel/preferences_hub.h"
#include "options/options_preferences_options.h"
#include "preferences/preferences_service.h"
#include "preferences/user_preferences_store.h"

#include "support/unit_test_hublocatorinjector.h"

namespace fs = std::filesystem;
using namespace AqualinkAutomate;

//=============================================================================
// Rejection and persistence-failure arms of the preferences subsystem.
//
// The preferences document is operator/UI supplied, so every field validator is
// a security-relevant gate: a malformed document must be rejected WHOLE (no
// partial application) with a stable error_code the UI can translate, and a
// persistence failure must never propagate out of the service.
//=============================================================================

namespace
{

	fs::path FreshPrefsDir(std::string_view tag)
	{
		static std::uint32_t counter{ 0 };
		const fs::path dir = fs::temp_directory_path() / std::format("aa-prefs-branches-{}-{}", tag, counter++);

		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::create_directories(dir, ec);

		return dir;
	}

	void WriteText(const fs::path& p, std::string_view text)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out << text;
	}

	std::string ReadText(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

}
// unnamed namespace

//=============================================================================
// PreferencesService — validation rejections
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_PreferencesServiceBranches, Test::HubLocatorInjector)

// A whole-document type error: preferences must be a JSON object, not an array
// or a scalar.
BOOST_AUTO_TEST_CASE(PrefsBranches_NonObjectDocumentRejected)
{
	Options::Preferences::PreferencesSettings settings;   // no file -> no persistence
	Preferences::PreferencesService service(*this, settings);

	std::string error;
	std::string error_code;

	BOOST_CHECK(!service.ApplyJson(nlohmann::json::array({ 1, 2, 3 }), error, error_code));
	BOOST_CHECK_EQUAL("prefs_not_object", error_code);
	BOOST_CHECK(!error.empty());

	BOOST_CHECK(!service.ApplyJson(nlohmann::json("a string"), error, error_code));
	BOOST_CHECK_EQUAL("prefs_not_object", error_code);
}

// Each field validator reports its OWN stable error_code (the UI maps these to
// translated messages), and a rejection applies nothing at all.
BOOST_AUTO_TEST_CASE(PrefsBranches_FieldRejectionsCarryStableErrorCodes)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);

	auto hub = Find<Kernel::PreferencesHub>();
	service.Seed(2800, 25, "https://hook.example/ok", 60);

	const auto salt_before = hub->AlertSaltLowPpm;
	const auto timeout_before = hub->AlertCommsTimeoutSeconds;
	const auto retention_before = hub->HistoryRetentionDays;

	const auto reject = [&](const nlohmann::json& doc, std::string_view expected_code)
		{
			std::string error;
			std::string error_code;
			BOOST_CHECK(!service.ApplyJson(doc, error, error_code));
			BOOST_CHECK_EQUAL(std::string{ expected_code }, error_code);
			BOOST_CHECK(!error.empty());
		};

	// Alert thresholds: out of range, wrong type, non-positive.
	reject(nlohmann::json{ { "alert", { { "salt_low_ppm", 99999 } } } }, "invalid_salt_low_ppm");
	reject(nlohmann::json{ { "alert", { { "salt_low_ppm", -1 } } } }, "invalid_salt_low_ppm");
	reject(nlohmann::json{ { "alert", { { "salt_low_ppm", "3000" } } } }, "invalid_salt_low_ppm");
	reject(nlohmann::json{ { "alert", { { "comms_timeout_seconds", 0 } } } }, "invalid_comms_timeout");
	reject(nlohmann::json{ { "alert", { { "comms_timeout_seconds", -5 } } } }, "invalid_comms_timeout");

	// Retention must be a positive integer.
	reject(nlohmann::json{ { "history", { { "retention_days", 0 } } } }, "invalid_retention_days");
	reject(nlohmann::json{ { "history", { { "retention_days", 1.5 } } } }, "invalid_retention_days");

	// Booleans and objects must actually be booleans and objects.
	reject(nlohmann::json{ { "show_aux_id_in_label", "yes" } }, "invalid_show_aux_id");
	reject(nlohmann::json{ { "ui", "not-an-object" } }, "invalid_ui");
	reject(nlohmann::json{ { "aux_presence_overrides", "not-an-object" } }, "invalid_aux_presence_overrides");
	reject(nlohmann::json{ { "aux_presence_overrides", { { "AUX1", "maybe" } } } }, "invalid_aux_presence_overrides");

	// Not one rejection changed live state.
	BOOST_CHECK_EQUAL(salt_before, hub->AlertSaltLowPpm);
	BOOST_CHECK_EQUAL(timeout_before, hub->AlertCommsTimeoutSeconds);
	BOOST_CHECK_EQUAL(retention_before, hub->HistoryRetentionDays);
}

// A webhook URL is a server-side outbound target: only absolute http(s) URLs
// with an authority are accepted.
BOOST_AUTO_TEST_CASE(PrefsBranches_WebhookUrlMustBeAbsoluteHttpUrl)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);

	const auto reject = [&](const char* url)
		{
			std::string error;
			std::string error_code;
			BOOST_CHECK_MESSAGE(!service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", url } } } }, error, error_code),
				std::string{ "expected rejection for " } + url);
		};

	reject("not-a-url");
	reject("ftp://example.com/hook");        // wrong scheme
	reject("file:///etc/passwd");            // wrong scheme, no authority
	reject("mailto:someone@example.com");    // no authority

	// The accepted shapes still apply.
	std::string error;
	BOOST_CHECK(service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", "http://example.com/hook" } } } }, error));
	BOOST_CHECK(service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", "https://example.com/hook" } } } }, error));

	// An empty URL disables the webhook rather than being rejected.
	BOOST_CHECK(service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", "" } } } }, error));
	BOOST_CHECK(Find<Kernel::PreferencesHub>()->AlertWebhookUrl.empty());
}

// Sections that are present but not objects are IGNORED (forwards-compatible)
// rather than rejected, so an older build reading a newer document keeps working.
BOOST_AUTO_TEST_CASE(PrefsBranches_NonObjectSectionsAreIgnored)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);

	auto hub = Find<Kernel::PreferencesHub>();
	service.Seed(2800, 25, "https://hook.example/ok", 60);

	std::string error;
	BOOST_CHECK(service.ApplyJson(nlohmann::json{ { "alert", 42 }, { "history", "nope" } }, error));

	BOOST_CHECK_EQUAL(2800u, hub->AlertSaltLowPpm);
	BOOST_CHECK_EQUAL(25u, hub->AlertCommsTimeoutSeconds);
	BOOST_CHECK_EQUAL(60u, hub->HistoryRetentionDays);
}

//=============================================================================
// PreferencesService — file load / save failure arms
//=============================================================================

BOOST_AUTO_TEST_CASE(PrefsBranches_MissingFileLeavesSeededValues)
{
	const fs::path dir = FreshPrefsDir("missing");

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = (dir / "does-not-exist.json").string();

	Preferences::PreferencesService service(*this, settings);
	service.Seed(2900, 45, "", 30);

	BOOST_CHECK_NO_THROW(service.Start());

	auto hub = Find<Kernel::PreferencesHub>();
	BOOST_CHECK_EQUAL(2900u, hub->AlertSaltLowPpm);
	BOOST_CHECK_EQUAL(45u, hub->AlertCommsTimeoutSeconds);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// A persisted file that parses but fails validation must be REJECTED wholesale,
// leaving the seeded values in place - not partially applied.
BOOST_AUTO_TEST_CASE(PrefsBranches_InvalidFileContentFallsBackToSeed)
{
	const fs::path dir = FreshPrefsDir("invalidcontent");
	const fs::path file = dir / "preferences.json";

	WriteText(file, R"({ "alert": { "salt_low_ppm": 99999, "comms_timeout_seconds": 15 } })");

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = file.string();

	Preferences::PreferencesService service(*this, settings);
	service.Seed(2900, 45, "", 30);

	BOOST_CHECK_NO_THROW(service.Start());

	auto hub = Find<Kernel::PreferencesHub>();
	BOOST_CHECK_EQUAL(2900u, hub->AlertSaltLowPpm);
	BOOST_CHECK_EQUAL(45u, hub->AlertCommsTimeoutSeconds);   // the valid sibling field is NOT applied either

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// A corrupt (unparseable) file must not take down bootstrap.
BOOST_AUTO_TEST_CASE(PrefsBranches_UnparseableFileIsSurvived)
{
	const fs::path dir = FreshPrefsDir("corrupt");
	const fs::path file = dir / "preferences.json";

	WriteText(file, "{ this is not json ");

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = file.string();

	Preferences::PreferencesService service(*this, settings);
	service.Seed(2900, 45, "", 30);

	BOOST_CHECK_NO_THROW(service.Start());

	BOOST_CHECK_EQUAL(2900u, Find<Kernel::PreferencesHub>()->AlertSaltLowPpm);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// A save that cannot complete (the target path is a directory, so the atomic
// rename fails) must be logged and swallowed: the in-memory preference still
// applies for the session.
BOOST_AUTO_TEST_CASE(PrefsBranches_UnwritableTargetDoesNotPropagate)
{
	const fs::path dir = FreshPrefsDir("unwritable");
	const fs::path blocked = dir / "preferences.json";

	std::error_code ec;
	fs::create_directories(blocked, ec);   // a DIRECTORY where the file should go
	BOOST_REQUIRE(fs::is_directory(blocked));

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = blocked.string();

	Preferences::PreferencesService service(*this, settings);

	std::string error;
	BOOST_CHECK_NO_THROW(service.ApplyJson(nlohmann::json{ { "alert", { { "salt_low_ppm", 3100 } } } }, error));

	// Applied live even though it could not be persisted.
	BOOST_CHECK_EQUAL(3100u, Find<Kernel::PreferencesHub>()->AlertSaltLowPpm);

	fs::remove_all(dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// UserPreferencesStore — rejection / degraded-file arms
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_UserPreferencesStoreBranches)

BOOST_AUTO_TEST_CASE(UserPrefsBranches_ApplyRejectsMissingUserAndNonObject)
{
	const fs::path dir = FreshPrefsDir("userapply");
	auto store = Preferences::UserPreferencesStore::Load(dir / "user_preferences.json");

	std::string error;

	// A per-user write with no identity has nowhere to go.
	BOOST_CHECK(!store.Apply("", nlohmann::json{ { "theme", "dark" } }, error));
	BOOST_CHECK(!error.empty());

	error.clear();
	BOOST_CHECK(!store.Apply("user-1", nlohmann::json::array({ "theme" }), error));
	BOOST_CHECK(!error.empty());

	BOOST_CHECK(!store.HasOverrides("user-1"));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// Every recognised per-user field is type- and value-checked; a rejection
// stores nothing.
BOOST_AUTO_TEST_CASE(UserPrefsBranches_EveryFieldValidatorRejects)
{
	const fs::path dir = FreshPrefsDir("uservalidate");
	auto store = Preferences::UserPreferencesStore::Load(dir / "user_preferences.json");

	const auto reject = [&](const nlohmann::json& partial)
		{
			std::string error;
			BOOST_CHECK(!store.Apply("user-1", partial, error));
			BOOST_CHECK(!error.empty());
		};

	reject(nlohmann::json{ { "temperature_units", "Kelvin" } });
	reject(nlohmann::json{ { "temperature_units", 3 } });
	reject(nlohmann::json{ { "theme", "neon" } });
	reject(nlohmann::json{ { "theme", true } });
	reject(nlohmann::json{ { "accent", "" } });
	reject(nlohmann::json{ { "accent", std::string(64, 'x') } });
	reject(nlohmann::json{ { "accent", 7 } });
	reject(nlohmann::json{ { "chemistry_bands", "not-an-object" } });

	BOOST_CHECK(!store.HasOverrides("user-1"));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// Defaults that are not an object (or that carry unknown keys) are ignored
// rather than poisoning every user's effective preferences.
BOOST_AUTO_TEST_CASE(UserPrefsBranches_DefaultsIgnoreNonObjectsAndUnknownKeys)
{
	const fs::path dir = FreshPrefsDir("userdefaults");
	auto store = Preferences::UserPreferencesStore::Load(dir / "user_preferences.json");

	store.SetDefaults(nlohmann::json::array({ "theme", "dark" }));
	BOOST_CHECK(store.Effective("user-1").empty());

	store.SetDefaults(nlohmann::json{
		{ "theme", "dark" },
		{ "AlertSaltLowPpm", 3000 },        // a SYSTEM field must never be adopted
		{ "not_a_preference", "value" } });

	const auto effective = store.Effective("user-1");
	BOOST_CHECK_EQUAL("dark", effective.at("theme").get<std::string>());
	BOOST_CHECK(!effective.contains("AlertSaltLowPpm"));
	BOOST_CHECK(!effective.contains("not_a_preference"));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

BOOST_AUTO_TEST_CASE(UserPrefsBranches_ForgetUnknownUserIsNoOp)
{
	const fs::path dir = FreshPrefsDir("userforget");
	const fs::path file = dir / "user_preferences.json";

	auto store = Preferences::UserPreferencesStore::Load(file);

	BOOST_CHECK_NO_THROW(store.Forget("never-existed"));
	BOOST_CHECK(!store.HasOverrides("never-existed"));

	// Nothing was written for a no-op forget.
	BOOST_CHECK(!fs::exists(file));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// With no file configured the store still works for the session; it simply
// never persists.
BOOST_AUTO_TEST_CASE(UserPrefsBranches_NoFileConfiguredSkipsPersistence)
{
	auto store = Preferences::UserPreferencesStore::Load(fs::path{});

	std::string error;
	BOOST_REQUIRE(store.Apply("user-1", nlohmann::json{ { "theme", "dark" } }, error));
	BOOST_CHECK(store.HasOverrides("user-1"));
	BOOST_CHECK_EQUAL("dark", store.Overrides("user-1").at("theme").get<std::string>());
}

// A file from a future (or foreign) schema must be refused loudly rather than
// silently reinterpreted.
BOOST_AUTO_TEST_CASE(UserPrefsBranches_UnsupportedSchemaVersionThrows)
{
	const fs::path dir = FreshPrefsDir("userschema");
	const fs::path file = dir / "user_preferences.json";

	// Produce a genuine file first so only the version differs from what this
	// build writes.
	{
		auto store = Preferences::UserPreferencesStore::Load(file);
		std::string error;
		BOOST_REQUIRE(store.Apply("user-1", nlohmann::json{ { "theme", "dark" } }, error));
	}
	BOOST_REQUIRE(fs::exists(file));

	auto document = nlohmann::json::parse(ReadText(file));
	document["schema_version"] = 9999;
	WriteText(file, document.dump());

	BOOST_CHECK_THROW(Preferences::UserPreferencesStore::Load(file), Exceptions::Preferences_StoreError);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// A structurally-valid file whose "users" map is the wrong type loads cleanly
// with no overrides (rather than throwing on every subsequent lookup).
BOOST_AUTO_TEST_CASE(UserPrefsBranches_MalformedUsersMapLoadsEmpty)
{
	const fs::path dir = FreshPrefsDir("usersmap");
	const fs::path file = dir / "user_preferences.json";

	{
		auto store = Preferences::UserPreferencesStore::Load(file);
		std::string error;
		BOOST_REQUIRE(store.Apply("user-1", nlohmann::json{ { "theme", "dark" } }, error));
	}

	auto document = nlohmann::json::parse(ReadText(file));
	document["users"] = "not-a-map";
	WriteText(file, document.dump());

	auto reloaded = Preferences::UserPreferencesStore::Load(file);
	BOOST_CHECK(!reloaded.HasOverrides("user-1"));

	// ...and a fresh write still succeeds over the top of it.
	std::string error;
	BOOST_CHECK(reloaded.Apply("user-1", nlohmann::json{ { "theme", "light" } }, error));
	BOOST_CHECK_EQUAL("light", reloaded.Overrides("user-1").at("theme").get<std::string>());

	std::error_code ec;
	fs::remove_all(dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()
