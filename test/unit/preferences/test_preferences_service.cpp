#include <filesystem>
#include <string>
#include <system_error>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "kernel/preferences_hub.h"
#include "options/options_preferences_options.h"
#include "preferences/preferences_service.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

BOOST_FIXTURE_TEST_SUITE(TestSuite_PreferencesService, Test::HubLocatorInjector)

BOOST_AUTO_TEST_CASE(Seed_SetsRuntimeFields)
{
	Options::Preferences::PreferencesSettings settings;   // no file
	Preferences::PreferencesService service(*this, settings);

	service.Seed(3000, 30, "https://hook.example/x", 45);

	auto json = service.ToJson();
	BOOST_CHECK_EQUAL(json["alert"]["salt_low_ppm"], 3000);
	BOOST_CHECK_EQUAL(json["alert"]["comms_timeout_seconds"], 30);
	BOOST_CHECK_EQUAL(json["alert"]["webhook_url"], "https://hook.example/x");
	BOOST_CHECK_EQUAL(json["history"]["retention_days"], 45);
}

BOOST_AUTO_TEST_CASE(ApplyJson_ValidUpdatesHubLive)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);

	nlohmann::json update = {
		{ "temperature_units", "Fahrenheit" },
		{ "alert", { { "salt_low_ppm", 2800 }, { "webhook_url", "http://x.local/hook" } } },
		{ "history", { { "retention_days", 14 } } },
		{ "ui", { { "chemistryBands", { { "ph", { { "goodMin", 7.4 } } } } } } },
	};

	std::string error;
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(update, error), error);

	auto hub = Find<Kernel::PreferencesHub>();
	BOOST_CHECK(hub->Temperature_DisplayUnits == Kernel::TemperatureUnits::Fahrenheit);
	BOOST_CHECK_EQUAL(hub->AlertSaltLowPpm, 2800u);
	BOOST_CHECK_EQUAL(hub->AlertWebhookUrl, "http://x.local/hook");
	BOOST_CHECK_EQUAL(hub->HistoryRetentionDays, 14u);
	// Opaque UI blob round-trips.
	BOOST_CHECK_EQUAL(hub->UiPreferences["chemistryBands"]["ph"]["goodMin"], 7.4);
}

// Regression: a PUT touching one ui.* key must not clobber the others — the
// blob merges shallowly at its top level (ui.chemistryBands and ui.locale are
// written by independent frontend features), and a null value deletes a key.
BOOST_AUTO_TEST_CASE(ApplyJson_UiMergesTopLevelKeys)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);

	std::string error;
	nlohmann::json first = { { "ui", { { "chemistryBands", { { "ph", { { "goodMin", 7.4 } } } } } } } };
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(first, error), error);

	nlohmann::json second = { { "ui", { { "locale", "fr" } } } };
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(second, error), error);

	auto hub = Find<Kernel::PreferencesHub>();
	BOOST_CHECK_EQUAL(hub->UiPreferences["locale"], "fr");
	BOOST_CHECK_EQUAL(hub->UiPreferences["chemistryBands"]["ph"]["goodMin"], 7.4);

	nlohmann::json third = { { "ui", { { "locale", nullptr } } } };
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(third, error), error);
	BOOST_CHECK(!hub->UiPreferences.contains("locale"));
	BOOST_CHECK(hub->UiPreferences.contains("chemistryBands"));
}

// DisplayUnitsChangedSignal fires exactly when Temperature_DisplayUnits
// changes value — consumers with published artefacts derived from it (the HA
// discovery setpoint number entities) republish on it. Re-applying the same
// value or touching unrelated fields must not fire it.
BOOST_AUTO_TEST_CASE(ApplyJson_UnitsChangeFiresSignal)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	auto hub = Find<Kernel::PreferencesHub>();

	int fired = 0;
	auto conn = hub->DisplayUnitsChangedSignal.connect([&fired]() { ++fired; });

	std::string error;
	nlohmann::json to_f = { { "temperature_units", "Fahrenheit" } };
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(to_f, error), error);
	BOOST_CHECK_EQUAL(fired, 1);

	BOOST_REQUIRE_MESSAGE(service.ApplyJson(to_f, error), error);
	BOOST_CHECK_EQUAL(fired, 1);

	nlohmann::json other = { { "history", { { "retention_days", 10 } } } };
	BOOST_REQUIRE_MESSAGE(service.ApplyJson(other, error), error);
	BOOST_CHECK_EQUAL(fired, 1);

	conn.disconnect();
}

BOOST_AUTO_TEST_CASE(ApplyJson_RejectsBadValues)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	std::string error;

	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "temperature_units", "Kelvin" } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "salt_low_ppm", 99999 } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "comms_timeout_seconds", 0 } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", "not-a-url" } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "history", { { "retention_days", 0 } } } }, error));
}

BOOST_AUTO_TEST_CASE(ApplyJson_RejectsWrongTypedFields_WithoutThrowing)
{
	// Regression: wrong-typed fields used to reach an unguarded nlohmann get<>()
	// (e.g. temperature_units / webhook_url as a non-string), throwing
	// nlohmann::json::type_error.  On the PUT /api/preferences path that throw
	// escaped into the router's exception barrier and surfaced as a blanket HTTP
	// 500.  ApplyJson must instead reject the payload (-> false -> HTTP 400) and
	// never throw.
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	std::string error;

	BOOST_CHECK_NO_THROW((void)service.ApplyJson(nlohmann::json{ { "temperature_units", 42 } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "temperature_units", 42 } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "temperature_units", true } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", 5 } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "webhook_url", nlohmann::json::array() } } } }, error));
}

BOOST_AUTO_TEST_CASE(ApplyJson_RejectionLeavesHubUnchanged)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	service.Seed(2600, 60, "", 90);

	std::string error;
	// salt is valid but comms is invalid -> the whole apply must be rejected.
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "alert", { { "salt_low_ppm", 1000 }, { "comms_timeout_seconds", 0 } } } }, error));

	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->AlertSaltLowPpm, 2600u);  // unchanged
}

BOOST_AUTO_TEST_CASE(ApplyJson_LabelOverridesRoundTripAndValidate)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	std::string error;

	// A valid canonical->friendly map applies and round-trips.
	BOOST_REQUIRE(service.ApplyJson(nlohmann::json{ { "label_overrides", { { "Aux1", "Pool Light" } } } }, error));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->LabelOverrides["Aux1"], "Pool Light");
	BOOST_CHECK_EQUAL(service.ToJson()["label_overrides"]["Aux1"], "Pool Light");

	// A non-string value is rejected and leaves the prior map intact.
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "label_overrides", { { "Aux1", 5 } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "label_overrides", "not-an-object" } }, error));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->LabelOverrides["Aux1"], "Pool Light");
}

BOOST_AUTO_TEST_CASE(ApplyJson_AuxPresenceOverridesRoundTripAndValidate)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	std::string error, error_code;

	// A valid aux-id->"present"/"absent" map applies and round-trips. PreferencesService only
	// persists the override here -- it does NOT reconcile the live device graph (that's a
	// Jandy-protocol-specific concern; this service lives in libaqualink-automate, which must
	// never depend on libaqualink-jandy). Reconciliation is covered separately by
	// Auxillaries::ApplyPresenceOverrides (test_jandy_auxillary_presence_override.cpp) and by
	// WebRoute_Equipment_AuxSlot, which calls it explicitly after a successful ApplyJson.
	BOOST_REQUIRE(service.ApplyJson(nlohmann::json{ { "aux_presence_overrides", { { "Aux5", "present" } } } }, error, error_code));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->AuxPresenceOverrides["Aux5"], "present");
	BOOST_CHECK_EQUAL(service.ToJson()["aux_presence_overrides"]["Aux5"], "present");

	// A value other than "present"/"absent" is rejected with a stable error_code, leaving the
	// prior map intact.
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "aux_presence_overrides", { { "Aux5", "on" } } } }, error, error_code));
	BOOST_CHECK_EQUAL(error_code, "invalid_aux_presence_overrides");
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "aux_presence_overrides", "not-an-object" } }, error, error_code));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->AuxPresenceOverrides["Aux5"], "present");

	// Clearing back to "auto" round-trips too.
	BOOST_REQUIRE(service.ApplyJson(nlohmann::json{ { "aux_presence_overrides", nlohmann::json::object() } }, error, error_code));
	BOOST_CHECK(Find<Kernel::PreferencesHub>()->AuxPresenceOverrides.empty());
}

BOOST_AUTO_TEST_CASE(SpaSwitchButtons_RoundTripAndValidate)
{
	Options::Preferences::PreferencesSettings settings;
	Preferences::PreferencesService service(*this, settings);
	std::string error;

	// A valid "switch:button"->function map applies and round-trips.
	BOOST_REQUIRE(service.ApplyJson(nlohmann::json{ { "spa_switch_buttons", { { "1:2", "Pool Light" }, { "2:1", "Swim Jet" } } } }, error));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->SpaSwitchButtons["1:2"], "Pool Light");
	BOOST_CHECK_EQUAL(service.ToJson()["spa_switch_buttons"]["2:1"], "Swim Jet");

	// Non-string value / non-object are rejected and leave the prior map intact.
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "spa_switch_buttons", { { "1:2", 7 } } } }, error));
	BOOST_CHECK(!service.ApplyJson(nlohmann::json{ { "spa_switch_buttons", "not-an-object" } }, error));
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->SpaSwitchButtons["1:2"], "Pool Light");
}

BOOST_AUTO_TEST_CASE(RecordSpaSwitchAssignment_StoresKeyedRequest)
{
	Options::Preferences::PreferencesSettings settings;   // no file -> Save() is a no-op, but the hub updates
	Preferences::PreferencesService service(*this, settings);

	service.RecordSpaSwitchAssignment(1, 2, "Spillway");
	service.RecordSpaSwitchAssignment(3, 4, "Air Blower");

	auto hub = Find<Kernel::PreferencesHub>();
	BOOST_CHECK_EQUAL(hub->SpaSwitchButtons["1:2"], "Spillway");
	BOOST_CHECK_EQUAL(hub->SpaSwitchButtons["3:4"], "Air Blower");
	// A re-record overwrites the same key.
	service.RecordSpaSwitchAssignment(1, 2, "Pool Light");
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->SpaSwitchButtons["1:2"], "Pool Light");
}

BOOST_AUTO_TEST_CASE(SpaSwitchButtons_FilePersistsAndReloads)
{
	const auto path = (std::filesystem::temp_directory_path() / "aqualink_prefs_spaswitch_test.json").string();
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = path;

	{
		Preferences::PreferencesService service(*this, settings);
		service.RecordSpaSwitchAssignment(2, 3, "Spa Jets");   // saves
	}

	Test::HubLocatorInjector fresh_locator;
	Preferences::PreferencesService reloaded(fresh_locator, settings);
	reloaded.Start();
	BOOST_CHECK_EQUAL(fresh_locator.Find<Kernel::PreferencesHub>()->SpaSwitchButtons["2:3"], "Spa Jets");

	std::filesystem::remove(path, ec);
	std::filesystem::remove(path + ".tmp", ec);
}

BOOST_AUTO_TEST_CASE(FileRoundTrip_PersistsAndReloads)
{
	const auto path = (std::filesystem::temp_directory_path() / "aqualink_prefs_test.json").string();
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Options::Preferences::PreferencesSettings settings;
	settings.preferences_file = path;

	{
		Preferences::PreferencesService service(*this, settings);
		std::string error;
		BOOST_REQUIRE(service.ApplyJson(nlohmann::json{ { "alert", { { "salt_low_ppm", 3300 } } } }, error));  // saves
	}

	// A fresh service + fresh hub loads the persisted value.
	Test::HubLocatorInjector fresh_locator;
	Preferences::PreferencesService reloaded(fresh_locator, settings);
	reloaded.Start();
	BOOST_CHECK_EQUAL(fresh_locator.Find<Kernel::PreferencesHub>()->AlertSaltLowPpm, 3300u);

	std::filesystem::remove(path, ec);
	std::filesystem::remove(path + ".tmp", ec);
}

BOOST_AUTO_TEST_SUITE_END()
