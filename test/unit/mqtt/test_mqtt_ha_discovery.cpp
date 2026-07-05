#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "alerting/alert_condition.h"
#include "mqtt/ha_discovery.h"
#include "mqtt/mqtt_client.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/data_hub.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

namespace
{
	Options::Mqtt::MqttSettings MakeTestSettings()
	{
		auto s = Test::MakeMqttSettings();
		s.topic_prefix = "aqualink";
		s.home_assistant_enabled = true;
		s.ha_discovery_prefix = "homeassistant";
		s.ha_device_id = "aqualink_aqualink";
		return s;
	}
}

//=============================================================================
// HA Discovery Slugify tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_Slugify)

BOOST_AUTO_TEST_CASE(Test_Slugify_BasicConversion)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Filter Pump"), "filter_pump");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_AllLowercase)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("pool"), "pool");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_AllUppercase)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("POOL HEATER"), "pool_heater");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_MixedCaseWithHyphen)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Spa-Heater"), "spa_heater");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_WithDots)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Pump.1"), "pump_1");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_MultipleSpaces)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Pool  Filter  Pump"), "pool_filter_pump");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_TrailingSpace)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Pump "), "pump");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_EmptyString)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify(""), "");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_SingleWord)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Chlorinator"), "chlorinator");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_SpecialCharsStripped)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("Aux #1 (Pool)"), "aux_1_pool");
}

BOOST_AUTO_TEST_CASE(Test_Slugify_NumericInput)
{
	BOOST_CHECK_EQUAL(Mqtt::HomeAssistantDiscovery::Slugify("123"), "123");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Discovery UniqueId tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_UniqueId)

BOOST_AUTO_TEST_CASE(Test_UniqueId_Format)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto id = ha.UniqueId("pool_temp");
	BOOST_CHECK_EQUAL(id, "aqualink_aqualink_pool_temp");
}

BOOST_AUTO_TEST_CASE(Test_UniqueId_DifferentPrefix)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	settings.topic_prefix = "my_pool";
	settings.ha_device_id = "aqualink_my_pool";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto id = ha.UniqueId("spa_temp");
	BOOST_CHECK_EQUAL(id, "aqualink_my_pool_spa_temp");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Discovery JSON object tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_JsonObjects)

BOOST_AUTO_TEST_CASE(Test_BuildDeviceObject_HasRequiredFields)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto device = ha.BuildDeviceObject();

	BOOST_CHECK(device.contains("identifiers"));
	BOOST_CHECK(device.contains("name"));
	BOOST_CHECK(device.contains("manufacturer"));
	BOOST_CHECK(device.contains("sw_version"));

	BOOST_CHECK_EQUAL(device["name"], "aqualink-automate");
	BOOST_CHECK_EQUAL(device["manufacturer"], "Jandy / Zodiac");

	auto identifiers = device["identifiers"];
	BOOST_REQUIRE(identifiers.is_array());
	BOOST_CHECK_EQUAL(identifiers.size(), 1);
	BOOST_CHECK_EQUAL(identifiers[0], "aqualink_aqualink");
}

BOOST_AUTO_TEST_CASE(Test_BuildDeviceObject_WithDataHub)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->EquipmentVersions.Set("Model", "RS-8");
	data_hub->EquipmentVersions.Set("Revision", "REV T.2");
	ha.ConnectDataHub(data_hub);

	auto device = ha.BuildDeviceObject();

	BOOST_CHECK(device.contains("model"));
	BOOST_CHECK_EQUAL(device["model"], "RS-8");
	BOOST_CHECK(device.contains("hw_version"));
	BOOST_CHECK_EQUAL(device["hw_version"], "REV T.2");
}

BOOST_AUTO_TEST_CASE(Test_BuildOriginObject_HasRequiredFields)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto origin = ha.BuildOriginObject();

	BOOST_CHECK(origin.contains("name"));
	BOOST_CHECK(origin.contains("sw_version"));
	BOOST_CHECK(origin.contains("support_url"));
	BOOST_CHECK_EQUAL(origin["name"], "aqualink-automate");
}

BOOST_AUTO_TEST_CASE(Test_BuildAvailability_HasCorrectStructure)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto availability = ha.BuildAvailability();

	BOOST_REQUIRE(availability.is_array());
	BOOST_REQUIRE_EQUAL(availability.size(), 1);

	auto entry = availability[0];
	BOOST_CHECK(entry.contains("topic"));
	BOOST_CHECK(entry.contains("payload_available"));
	BOOST_CHECK(entry.contains("payload_not_available"));
	BOOST_CHECK_EQUAL(entry["topic"], "aqualink/status/availability");
	BOOST_CHECK_EQUAL(entry["payload_available"], "online");
	BOOST_CHECK_EQUAL(entry["payload_not_available"], "offline");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Discovery topic tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_Topics)

BOOST_AUTO_TEST_CASE(Test_AvailabilityTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	BOOST_CHECK_EQUAL(ha.AvailabilityTopic(), "aqualink/status/availability");
}

BOOST_AUTO_TEST_CASE(Test_TemperaturesTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	BOOST_CHECK_EQUAL(ha.TemperaturesTopic(), "aqualink/pool/temperatures");
}

BOOST_AUTO_TEST_CASE(Test_ChemistryTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	BOOST_CHECK_EQUAL(ha.ChemistryTopic(), "aqualink/pool/chemistry");
}

BOOST_AUTO_TEST_CASE(Test_CirculationTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	BOOST_CHECK_EQUAL(ha.CirculationTopic(), "aqualink/pool/circulation");
}

BOOST_AUTO_TEST_CASE(Test_SystemStatusTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	BOOST_CHECK_EQUAL(ha.SystemStatusTopic(), "aqualink/system/status");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Discovery publish flow tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_Publish)

BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_QueuesToClient)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);

	// Device discovery publishes a single payload
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	// Discovery config should be retained
	BOOST_CHECK(queue[0].retain);

	// Verify discovery topic format
	BOOST_CHECK_EQUAL(queue[0].topic, "homeassistant/device/aqualink_aqualink/config");

	// Parse the payload and verify cmps has at least 13 components
	auto payload = nlohmann::json::parse(queue[0].payload);
	BOOST_REQUIRE(payload.contains("cmps"));
	BOOST_CHECK_GE(payload["cmps"].size(), 13);
}

// Every AlertMonitor condition is exposed as a Home Assistant problem
// binary_sensor reading the shared alert-state topic via a per-condition
// value_template (WS3).
BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_AlertProblemBinarySensors)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	auto payload = nlohmann::json::parse(queue[0].payload);
	BOOST_REQUIRE(payload.contains("cmps"));
	auto& cmps = payload["cmps"];

	const std::string state_topic = ha.AlertStateTopic();

	for (const auto& condition : Alerting::AlertConditions)
	{
		const std::string comp_key = std::string("alert_") + std::string(condition.key);
		BOOST_REQUIRE_MESSAGE(cmps.contains(comp_key), "missing alert component: " + comp_key);

		auto& cmp = cmps[comp_key];
		BOOST_CHECK_EQUAL(cmp["p"], "binary_sensor");
		BOOST_CHECK_EQUAL(cmp["device_class"], "problem");
		BOOST_CHECK_EQUAL(cmp["state_topic"], state_topic);
		BOOST_CHECK_EQUAL(cmp["payload_on"], "true");
		BOOST_CHECK_EQUAL(cmp["payload_off"], "false");

		const std::string expected_template = std::string("{{ value_json.") + std::string(condition.key) + " }}";
		BOOST_CHECK_EQUAL(cmp["value_template"], expected_template);
	}
}

BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_ValidJson)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	BOOST_REQUIRE(payload.contains("cmps"));

	for (auto& [key, cmp] : payload["cmps"].items())
	{
		BOOST_CHECK_MESSAGE(cmp.contains("p"), "Component missing 'p' (platform): " + key);
		BOOST_CHECK_MESSAGE(cmp.contains("name"), "Component missing 'name': " + key);
		BOOST_CHECK_MESSAGE(cmp.contains("unique_id"), "Component missing 'unique_id': " + key);
		BOOST_CHECK_MESSAGE(cmp.contains("state_topic"), "Component missing 'state_topic': " + key);
	}
}

BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_TemperatureSensorsHaveCorrectClass)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	int temp_sensor_count = 0;
	int temp_number_count = 0;
	for (auto& [key, cmp] : cmps.items())
	{
		if (cmp.contains("device_class") && cmp["device_class"] == "temperature")
		{
			BOOST_CHECK_EQUAL(cmp["unit_of_measurement"], "\u00B0C");
			BOOST_CHECK_EQUAL(cmp["state_topic"], "aqualink/pool/temperatures");

			auto platform = cmp["p"].get<std::string>();
			if (platform == "sensor")
			{
				++temp_sensor_count;
				BOOST_CHECK_EQUAL(cmp["state_class"], "measurement");
			}
			else if (platform == "number")
			{
				++temp_number_count;
				BOOST_CHECK_MESSAGE(cmp.contains("command_topic"), "number entity missing 'command_topic': " + key);
				BOOST_CHECK_MESSAGE(cmp.contains("min"), "number entity missing 'min': " + key);
				BOOST_CHECK_MESSAGE(cmp.contains("max"), "number entity missing 'max': " + key);
				BOOST_CHECK_MESSAGE(cmp.contains("step"), "number entity missing 'step': " + key);
			}
		}
	}

	// Should have 5 temperature sensors (pool, spa, air, freeze_protect, pool_setpoint_2 [read-only
	// TEMP2]) + 2 number entities (pool_setpoint, spa_setpoint)
	BOOST_CHECK_EQUAL(temp_sensor_count, 5);
	BOOST_CHECK_EQUAL(temp_number_count, 2);
}

BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_BinarySensorsHavePayloadOnOff)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	for (auto& [key, cmp] : cmps.items())
	{
		if (cmp.contains("p") && cmp["p"] == "binary_sensor")
		{
			BOOST_CHECK_MESSAGE(cmp.contains("payload_on"),
				"binary_sensor component missing 'payload_on': " + key);
			BOOST_CHECK_MESSAGE(cmp.contains("payload_off"),
				"binary_sensor component missing 'payload_off': " + key);
		}
	}
}

// Regression: a binary_sensor whose value_template is a raw {{ value_json.x }}
// passthrough renders a JSON boolean source as Python-capitalised "True"/"False",
// which never matches the lowercase payload_on/payload_off ("true"/"false"). HA then
// logs "No matching payload found" on every publish and the entity never settles.
// The fix is to coerce inside the template ({{ 'true' if value_json.x else 'false' }}).
//
// This test (a) pins the three previously-broken bool sensors to the coercion form, and
// (b) walks EVERY generated binary_sensor so any future entity that reintroduces the
// raw-passthrough + lowercase-payload combo fails loudly here.
BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_BooleanBinarySensorsCoerceLowercase)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	// A combo system so the spa/clean circulation sensors and both bodies are emitted.
	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::UserSpecified);
	ha.ConnectDataHub(data_hub);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	auto payload = nlohmann::json::parse(queue[0].payload);
	BOOST_REQUIRE(payload.contains("cmps"));
	auto& cmps = payload["cmps"];

	// A "raw passthrough" template emits the JSON value verbatim - no conditional, no
	// filter, no explicit lowercase literal - so a bool source stringifies to "True"/"False".
	auto is_raw_passthrough = [](const std::string& tmpl)
	{
		return tmpl.find("value_json") != std::string::npos
			&& tmpl.find(" if ") == std::string::npos
			&& tmpl.find('|') == std::string::npos
			&& tmpl.find("'true'") == std::string::npos
			&& tmpl.find("'false'") == std::string::npos;
	};

	// Documented exception: the alert binary_sensors read the AlertMonitor's consolidated
	// document, which already publishes the STRING "true"/"false" (AlertMonitor::BuildStateJson),
	// so a raw passthrough is correct there. They must NOT be "fixed" with the truthiness
	// coercion - {{ 'true' if value_json.x else 'false' }} would render the string "false" as
	// truthy and report a cleared alert as raised. Any other raw-passthrough bool sensor is a bug.
	auto is_string_sourced_exception = [](const std::string& key)
	{
		return key.rfind("alert_", 0) == 0;
	};

	std::size_t bool_sensor_count = 0;
	for (auto& [key, cmp] : cmps.items())
	{
		if (!cmp.contains("p") || cmp["p"] != "binary_sensor")
		{
			continue;
		}

		BOOST_REQUIRE_MESSAGE(cmp.contains("payload_on") && cmp.contains("payload_off"),
			"binary_sensor missing payload_on/off: " + key);

		const auto payload_on = cmp["payload_on"].get<std::string>();
		const auto payload_off = cmp["payload_off"].get<std::string>();

		// We only police the lowercase-boolean payload convention here.
		if (!(payload_on == "true" && payload_off == "false"))
		{
			continue;
		}

		BOOST_REQUIRE_MESSAGE(cmp.contains("value_template"),
			"bool binary_sensor missing value_template: " + key);
		const auto tmpl = cmp["value_template"].get<std::string>();

		if (is_string_sourced_exception(key))
		{
			continue;
		}

		++bool_sensor_count;

		// The combo we are guarding against: a raw passthrough whose output would be
		// "True"/"False" for a JSON-bool source.
		BOOST_CHECK_MESSAGE(!is_raw_passthrough(tmpl),
			"binary_sensor '" + key + "' uses a raw {{ value_json.x }} passthrough with lowercase "
			"payload_on/off; a JSON-bool source renders 'True'/'False' and never matches. Coerce in "
			"the template: {{ 'true' if value_json.x else 'false' }} (value_template was: " + tmpl + ")");

		// And, positively, the template must emit literals that match the declared payloads.
		BOOST_CHECK_MESSAGE(tmpl.find("'" + payload_on + "'") != std::string::npos,
			"binary_sensor '" + key + "' value_template does not emit payload_on literal '" + payload_on + "': " + tmpl);
		BOOST_CHECK_MESSAGE(tmpl.find("'" + payload_off + "'") != std::string::npos,
			"binary_sensor '" + key + "' value_template does not emit payload_off literal '" + payload_off + "': " + tmpl);
	}

	// Sanity: the walk actually exercised the policed sensors (3 circulation/setpoint bool
	// sensors + 3 temperature "stale" companions on a combo system), not zero.
	BOOST_CHECK_GE(bool_sensor_count, 6u);

	// Pin the three previously-broken sensors to the exact coercion form.
	for (const auto* key : { "spa_mode", "clean_mode", "pool_heater_2_enabled" })
	{
		BOOST_REQUIRE_MESSAGE(cmps.contains(key), std::string("missing bool binary_sensor: ") + key);
		auto& cmp = cmps[key];
		BOOST_CHECK_EQUAL(cmp["p"], "binary_sensor");
		BOOST_CHECK_EQUAL(cmp["payload_on"], "true");
		BOOST_CHECK_EQUAL(cmp["payload_off"], "false");

		const auto tmpl = cmp["value_template"].get<std::string>();
		const std::string expected = std::string("{{ 'true' if value_json.") + key + " else 'false' }}";
		BOOST_CHECK_EQUAL(tmpl, expected);
	}
}

BOOST_AUTO_TEST_CASE(Test_PublishDiscoveryConfigs_SwitchEntitiesHaveCommandTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	// Create a data hub with a pump device so we get a switch entity
	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	auto pump = std::make_shared<Kernel::AuxillaryDevice>();
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Pump);
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, std::string("Filter Pump"));
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Off);
	data_hub->Devices.Add(pump);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	int switch_count = 0;
	for (auto& [key, cmp] : cmps.items())
	{
		if (cmp.contains("p") && cmp["p"] == "switch")
		{
			++switch_count;
			BOOST_CHECK_MESSAGE(cmp.contains("command_topic"),
				"switch component missing 'command_topic': " + key);
			BOOST_CHECK_MESSAGE(cmp.contains("state_on"),
				"switch component missing 'state_on': " + key);
			BOOST_CHECK_MESSAGE(cmp.contains("state_off"),
				"switch component missing 'state_off': " + key);
			BOOST_CHECK_MESSAGE(cmp.contains("payload_on"),
				"switch component missing 'payload_on': " + key);
			BOOST_CHECK_MESSAGE(cmp.contains("payload_off"),
				"switch component missing 'payload_off': " + key);
			BOOST_CHECK_EQUAL(cmp["payload_on"], "ON");
			BOOST_CHECK_EQUAL(cmp["payload_off"], "OFF");
		}
	}

	BOOST_CHECK_GE(switch_count, 1);
}

BOOST_AUTO_TEST_CASE(Test_PublishOnline_PublishesRetainedOnline)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishOnline();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	BOOST_CHECK_EQUAL(queue[0].topic, "aqualink/status/availability");
	BOOST_CHECK_EQUAL(queue[0].payload, "online");
	BOOST_CHECK_EQUAL(queue[0].retain, true);
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_NoDataHub_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	// No DataHub connected - should not crash
	BOOST_CHECK_NO_THROW(ha.PublishDeviceStates());

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_CHECK_EQUAL(queue.size(), 0);
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_EmptyDataHub_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	// Empty data hub (no devices) - should not crash
	BOOST_CHECK_NO_THROW(ha.PublishDeviceStates());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Config-respecting entity gating + temperature freshness companions
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_ConfigGating)

namespace
{
	nlohmann::json PublishAndGetComponents(Mqtt::HomeAssistantDiscovery& ha, Mqtt::MqttClient& client)
	{
		ha.PublishDiscoveryConfigs();
		auto& queue = Test::MqttClientPacketTest::GetPublishQueue(client);
		BOOST_REQUIRE_EQUAL(queue.size(), 1);
		return nlohmann::json::parse(queue[0].payload)["cmps"];
	}
}

BOOST_AUTO_TEST_CASE(Test_Gating_PoolOnly_HidesSpaEntities)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::SingleBody, Kernel::ConfigurationSource::UserSpecified, Kernel::BodyOfWaterIds::Pool);
	ha.ConnectDataHub(data_hub);

	auto cmps = PublishAndGetComponents(ha, *client);

	BOOST_CHECK(cmps.contains("pool_temp"));
	BOOST_CHECK(cmps.contains("pool_setpoint"));
	BOOST_CHECK(!cmps.contains("spa_temp"));
	BOOST_CHECK(!cmps.contains("spa_setpoint"));
	// Air + freeze are system-wide and always present.
	BOOST_CHECK(cmps.contains("air_temp"));
	BOOST_CHECK(cmps.contains("freeze_protect_temp"));
}

BOOST_AUTO_TEST_CASE(Test_Gating_SpaOnly_HidesPoolEntities)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::SingleBody, Kernel::ConfigurationSource::UserSpecified, Kernel::BodyOfWaterIds::Spa);
	ha.ConnectDataHub(data_hub);

	auto cmps = PublishAndGetComponents(ha, *client);

	BOOST_CHECK(cmps.contains("spa_temp"));
	BOOST_CHECK(cmps.contains("spa_setpoint"));
	BOOST_CHECK(!cmps.contains("pool_temp"));
	BOOST_CHECK(!cmps.contains("pool_setpoint"));
}

BOOST_AUTO_TEST_CASE(Test_Gating_Combo_ShowsBothBodies)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::UserSpecified);
	ha.ConnectDataHub(data_hub);

	auto cmps = PublishAndGetComponents(ha, *client);

	BOOST_CHECK(cmps.contains("pool_temp"));
	BOOST_CHECK(cmps.contains("spa_temp"));
	BOOST_CHECK(cmps.contains("pool_setpoint"));
	BOOST_CHECK(cmps.contains("spa_setpoint"));
}

BOOST_AUTO_TEST_CASE(Test_Gating_NoDataHub_EmitsBoth)
{
	// Defensive fallback: with no DataHub (config unknown) both bodies' entities are emitted so
	// nothing is missing during discovery.
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto cmps = PublishAndGetComponents(ha, *client);

	BOOST_CHECK(cmps.contains("pool_temp"));
	BOOST_CHECK(cmps.contains("spa_temp"));
}

BOOST_AUTO_TEST_CASE(Test_Freshness_CompanionsPresent)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::UserSpecified);
	ha.ConnectDataHub(data_hub);

	auto cmps = PublishAndGetComponents(ha, *client);

	// Each live temperature gets a "last updated" timestamp sensor and a "stale" problem sensor.
	for (const auto* base : { "pool_temp", "spa_temp", "air_temp" })
	{
		auto updated_key = std::string(base) + "_updated";
		auto stale_key = std::string(base) + "_stale";
		BOOST_CHECK_MESSAGE(cmps.contains(updated_key), "missing " + updated_key);
		BOOST_CHECK_MESSAGE(cmps.contains(stale_key), "missing " + stale_key);
		BOOST_CHECK_EQUAL(cmps[updated_key]["device_class"], "timestamp");
		BOOST_CHECK_EQUAL(cmps[stale_key]["device_class"], "problem");
	}

	// Freeze-protect and setpoints are configured values - no stale companion.
	BOOST_CHECK(!cmps.contains("freeze_protect_temp_stale"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Retained-topic lifecycle: a device that drops out must have its discovery component
// tombstoned (so HA deletes the entity) and its retained state topic cleared.
//=============================================================================

namespace
{
	std::shared_ptr<Kernel::AuxillaryDevice> MakeAuxOn(const std::string& label)
	{
		namespace Traits = Kernel::AuxillaryTraitsTypes;
		auto aux = std::make_shared<Kernel::AuxillaryDevice>();
		aux->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		aux->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		aux->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);
		return aux;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_RetainedLifecycle)

BOOST_AUTO_TEST_CASE(Test_RemovedDevice_TombstonesDiscoveryComponent)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);
	auto aux = MakeAuxOn("Aux5");
	data_hub->Devices.Add(aux);

	// First publish: the aux switch component is present and fully populated.
	ha.PublishDiscoveryConfigs();
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	{
		auto cmps = nlohmann::json::parse(queue.back().payload)["cmps"];
		BOOST_REQUIRE_MESSAGE(cmps.contains("aux_aux5"), "aux switch component was not published");
		BOOST_CHECK(cmps["aux_aux5"].contains("command_topic"));
	}

	// Remove the device and republish: the component is tombstoned (reduced to just its platform)
	// so Home Assistant deletes the entity rather than leaving a stale/duplicate one.
	data_hub->Devices.Remove(aux);
	ha.PublishDiscoveryConfigs();

	auto cmps = nlohmann::json::parse(queue.back().payload)["cmps"];
	BOOST_REQUIRE(cmps.contains("aux_aux5"));
	BOOST_CHECK_EQUAL(cmps["aux_aux5"].size(), 1u);          // only the platform key remains
	BOOST_CHECK_EQUAL(cmps["aux_aux5"]["p"], "switch");
}

BOOST_AUTO_TEST_CASE(Test_RemovedDevice_ClearsRetainedStateTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);
	auto aux = MakeAuxOn("Aux5");
	data_hub->Devices.Add(aux);

	ha.PublishDeviceStates();
	data_hub->Devices.Remove(aux);
	ha.PublishDeviceStates();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);

	// The last publish to the aux state topic clears it (empty + retained).
	const std::string suffix = "ha/aux_aux5";
	std::optional<std::pair<std::string, bool>> last;
	for (const auto& pending : queue)
	{
		if (pending.topic.size() >= suffix.size()
			&& 0 == pending.topic.compare(pending.topic.size() - suffix.size(), suffix.size(), suffix))
		{
			last = std::make_pair(pending.payload, pending.retain);
		}
	}
	BOOST_REQUIRE_MESSAGE(last.has_value(), "aux state topic was never published");
	BOOST_CHECK_MESSAGE(last->first.empty(), "vanished state topic should be cleared with an empty payload");
	BOOST_CHECK_MESSAGE(last->second, "the clearing publish must be retained");
}

BOOST_AUTO_TEST_CASE(Test_AdoptRetainedComponents_TombstonesEntitiesGoneSinceLastRun)
{
	// Startup broker reconciliation for HA discovery: adopting the broker's EXISTING retained config
	// (from a prior run that had an "aux_aux5" switch) means the next config publish tombstones that
	// entity, since no such device exists now - removing the ghost entity from Home Assistant.
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);   // no aux devices this run

	nlohmann::json old_config;
	old_config["cmps"]["aux_aux5"] = { {"p", "switch"}, {"name", "Aux5"}, {"command_topic", "test/command/device/aux5"} };
	old_config["cmps"]["orp"] = { {"p", "sensor"}, {"name", "ORP"} };   // a static entity still emitted
	ha.AdoptRetainedComponents(old_config.dump());

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	auto cmps = nlohmann::json::parse(queue.back().payload)["cmps"];

	// The device entity from the old run, no longer present, is tombstoned (platform-only).
	BOOST_REQUIRE(cmps.contains("aux_aux5"));
	BOOST_CHECK_EQUAL(cmps["aux_aux5"].size(), 1u);
	BOOST_CHECK_EQUAL(cmps["aux_aux5"]["p"], "switch");

	// A static entity that is still emitted this run is NOT tombstoned.
	BOOST_REQUIRE(cmps.contains("orp"));
	BOOST_CHECK_GT(cmps["orp"].size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Device Discovery payload format tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_DevicePayload)

BOOST_AUTO_TEST_CASE(Test_DeviceDiscoveryTopic_Format)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	BOOST_CHECK_EQUAL(queue[0].topic, "homeassistant/device/aqualink_aqualink/config");
}

BOOST_AUTO_TEST_CASE(Test_DeviceDiscoveryPayload_HasRootFields)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	BOOST_CHECK(payload.contains("dev"));
	BOOST_CHECK(payload.contains("o"));
	BOOST_CHECK(payload.contains("availability"));
	BOOST_CHECK(payload.contains("cmps"));
}

BOOST_AUTO_TEST_CASE(Test_DeviceDiscoveryPayload_NoDuplicateDeviceInComponents)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	for (auto& [key, cmp] : cmps.items())
	{
		BOOST_CHECK_MESSAGE(!cmp.contains("device"),
			"Component should not contain 'device' (it's at root level): " + key);
		BOOST_CHECK_MESSAGE(!cmp.contains("origin"),
			"Component should not contain 'origin' (it's at root level): " + key);
		BOOST_CHECK_MESSAGE(!cmp.contains("availability"),
			"Component should not contain 'availability' (it's at root level): " + key);
	}
}

BOOST_AUTO_TEST_CASE(Test_DeviceDiscoveryPayload_AllComponentsHavePlatform)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	for (auto& [key, cmp] : cmps.items())
	{
		BOOST_CHECK_MESSAGE(cmp.contains("p"),
			"Component missing 'p' (platform): " + key);

		auto platform = cmp["p"].get<std::string>();
		BOOST_CHECK_MESSAGE(platform == "sensor" || platform == "binary_sensor" || platform == "number" || platform == "switch",
			std::string("Component has unexpected platform '").append(platform).append("': ").append(key));
	}
}

BOOST_AUTO_TEST_CASE(Test_CustomDeviceId)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	settings.ha_device_id = "custom_pool_1";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	// Verify topic uses custom device ID
	BOOST_CHECK_EQUAL(queue[0].topic, "homeassistant/device/custom_pool_1/config");

	// Verify device identifier uses custom device ID
	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& identifiers = payload["dev"]["identifiers"];
	BOOST_REQUIRE(identifiers.is_array());
	BOOST_CHECK_EQUAL(identifiers[0], "custom_pool_1");

	// Verify unique_ids use custom device ID prefix
	auto& cmps = payload["cmps"];
	BOOST_REQUIRE(cmps.contains("pool_temp"));
	BOOST_CHECK_EQUAL(cmps["pool_temp"]["unique_id"], "custom_pool_1_pool_temp");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttClient retain flag tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClient_Retain)

BOOST_AUTO_TEST_CASE(Test_Publish_DefaultRetainIsFalse)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Publish("test/topic", "payload");

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	BOOST_CHECK_EQUAL(queue[0].retain, false);
}

BOOST_AUTO_TEST_CASE(Test_Publish_RetainTrue)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Publish("test/topic", "payload", true);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	BOOST_CHECK_EQUAL(queue[0].retain, true);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttClient LWT tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClient_LWT)

BOOST_AUTO_TEST_CASE(Test_SetWill_StoresConfig)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	Mqtt::MqttClient client(ioc, settings);

	BOOST_CHECK(!client.GetWill().has_value());

	client.SetWill("test/status/availability", "offline", true);

	auto& will = client.GetWill();
	BOOST_REQUIRE(will.has_value());
	BOOST_CHECK_EQUAL(will->topic, "test/status/availability");
	BOOST_CHECK_EQUAL(will->payload, "offline");
	BOOST_CHECK_EQUAL(will->retain, true);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttSettings HA defaults tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttSettings_HA)

BOOST_AUTO_TEST_CASE(Test_MqttSettings_HaDefaultsDisabled)
{
	Options::Mqtt::MqttSettings settings;
	BOOST_CHECK_EQUAL(settings.home_assistant_enabled, false);
	BOOST_CHECK_EQUAL(settings.ha_discovery_prefix, "homeassistant");
	BOOST_CHECK(settings.ha_device_id.empty());
}

BOOST_AUTO_TEST_CASE(Test_MqttSettings_HaEnabled)
{
	Options::Mqtt::MqttSettings settings;
	settings.home_assistant_enabled = true;
	settings.ha_discovery_prefix = "ha_test";

	BOOST_CHECK_EQUAL(settings.home_assistant_enabled, true);
	BOOST_CHECK_EQUAL(settings.ha_discovery_prefix, "ha_test");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HA Discovery config discovery prefix tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_CustomPrefix)

BOOST_AUTO_TEST_CASE(Test_CustomDiscoveryPrefix)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	settings.ha_discovery_prefix = "my_ha";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	BOOST_CHECK_MESSAGE(
		queue[0].topic.find("my_ha/device/") == 0,
		"Discovery topic should start with 'my_ha/device/': " + queue[0].topic);
}

BOOST_AUTO_TEST_CASE(Test_CustomTopicPrefix)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	settings.topic_prefix = "mypool";
	settings.ha_device_id = "aqualink_mypool";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	// Parse the single payload and check state_topic in each component
	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	for (auto& [key, cmp] : cmps.items())
	{
		auto state_topic = cmp["state_topic"].get<std::string>();
		BOOST_CHECK_MESSAGE(
			state_topic.find("mypool/") == 0,
			"State topic should use custom prefix 'mypool/': " + state_topic);
	}
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Setpoint number entities follow the Temperature_DisplayUnits preference
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_SetpointUnits)

namespace
{
	nlohmann::json PublishAndGetComponentsForUnits(Mqtt::HomeAssistantDiscovery& ha, Mqtt::MqttClient& client)
	{
		ha.PublishDiscoveryConfigs();
		auto& queue = Test::MqttClientPacketTest::GetPublishQueue(client);
		BOOST_REQUIRE_GE(queue.size(), 1);
		return nlohmann::json::parse(queue.back().payload)["cmps"];
	}
}

// Default / no preferences hub connected: Celsius entity, exactly as before.
BOOST_AUTO_TEST_CASE(Test_SetpointUnits_DefaultCelsius)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto cmps = PublishAndGetComponentsForUnits(ha, *client);

	BOOST_REQUIRE(cmps.contains("pool_setpoint"));
	const auto& sp = cmps["pool_setpoint"];
	BOOST_CHECK_EQUAL(sp["unit_of_measurement"], "°C");
	BOOST_CHECK(sp["value_template"].get<std::string>().find(".celsius") != std::string::npos);
	BOOST_CHECK_EQUAL(15.0, sp["min"].get<double>());
	BOOST_CHECK_EQUAL(41.0, sp["max"].get<double>());
	BOOST_CHECK_EQUAL(0.5, sp["step"].get<double>());
}

// Fahrenheit preference: the NUMBER entities switch template/unit/range (HA
// does not convert number entities to its own unit system) while the
// temperature SENSORS stay declared degC (their payload IS celsius; HA
// converts sensors itself).
BOOST_AUTO_TEST_CASE(Test_SetpointUnits_FahrenheitPreference)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto preferences_hub = std::make_shared<Kernel::PreferencesHub>();
	preferences_hub->Temperature_DisplayUnits = Kernel::TemperatureUnits::Fahrenheit;
	ha.ConnectPreferencesHub(preferences_hub);

	auto cmps = PublishAndGetComponentsForUnits(ha, *client);

	BOOST_REQUIRE(cmps.contains("pool_setpoint"));
	const auto& sp = cmps["pool_setpoint"];
	BOOST_CHECK_EQUAL(sp["unit_of_measurement"], "°F");
	BOOST_CHECK(sp["value_template"].get<std::string>().find(".fahrenheit") != std::string::npos);
	BOOST_CHECK_EQUAL(59.0, sp["min"].get<double>());
	BOOST_CHECK_EQUAL(106.0, sp["max"].get<double>());
	BOOST_CHECK_EQUAL(1.0, sp["step"].get<double>());

	BOOST_REQUIRE(cmps.contains("air_temp"));
	BOOST_CHECK_EQUAL(cmps["air_temp"]["unit_of_measurement"], "°C");
	BOOST_CHECK(cmps["air_temp"]["value_template"].get<std::string>().find(".celsius") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// PublishDeviceStates across every device category, plus the no-label skip path
// (a device with no LabelTrait must be silently skipped, never crash).
//=============================================================================

namespace
{
	std::shared_ptr<Kernel::AuxillaryDevice> MakeTypedDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes type, const std::string& label)
	{
		namespace Traits = Kernel::AuxillaryTraitsTypes;
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		return dev;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_DeviceStates)

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_AllCategories_PublishRetainedState)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Pump, "Filter Pump"));
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Heater, "Pool Heater"));
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Chlorinator, "AquaPure"));
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Auxillary, "Pool Light"));

	ha.PublishDeviceStates();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);

	// Each category publishes a retained short-state topic under ha/.
	auto has_topic = [&](const std::string& topic)
	{
		for (const auto& pending : queue)
		{
			if (pending.topic == topic) { return true; }
		}
		return false;
	};

	BOOST_CHECK(has_topic("aqualink/ha/pump_filter_pump"));
	BOOST_CHECK(has_topic("aqualink/ha/heater_pool_heater"));
	BOOST_CHECK(has_topic("aqualink/ha/chlorinator_aquapure"));
	BOOST_CHECK(has_topic("aqualink/ha/aux_pool_light"));
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_UnlabelledDevice_Skipped)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	// A pump with no LabelTrait: publish_device_state must skip it (no slug can be formed).
	auto unlabelled = std::make_shared<Kernel::AuxillaryDevice>();
	unlabelled->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Pump);
	data_hub->Devices.Add(unlabelled);

	BOOST_CHECK_NO_THROW(ha.PublishDeviceStates());

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	std::size_t state_topics = 0;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/ha/") != std::string::npos) { ++state_topics; }
	}
	BOOST_CHECK_EQUAL(state_topics, 0u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// AddDynamicDeviceComponents no-label skip paths: an unlabelled heater and an
// unlabelled chlorinator must be silently skipped (no component emitted, no
// crash) — the heater-specific continue and the AddChlorinatorComponents early
// return respectively.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_UnlabelledDynamicDevices)

BOOST_AUTO_TEST_CASE(Test_DynamicComponents_UnlabelledHeaterAndChlorinator_Skipped)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	// A heater and a chlorinator with NO LabelTrait: the dynamic-component builder must skip both
	// (no slug can be formed) without emitting a switch/sensor for them.
	auto heater = std::make_shared<Kernel::AuxillaryDevice>();
	heater->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Heater);
	data_hub->Devices.Add(heater);

	auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
	chlorinator->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Chlorinator);
	data_hub->Devices.Add(chlorinator);

	// A labelled aux so the discovery build still completes with real components.
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Auxillary, "Deck Jets"));

	BOOST_CHECK_NO_THROW(ha.PublishDiscoveryConfigs());

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);
	auto cmps = nlohmann::json::parse(queue[0].payload)["cmps"];

	// The labelled aux is present; no unlabelled heater/chlorinator components leaked in.
	BOOST_CHECK(cmps.contains("aux_deck_jets"));
	for (auto& [key, cmp] : cmps.items())
	{
		BOOST_CHECK_MESSAGE(key.rfind("heater_", 0) != 0, "no heater component should exist for an unlabelled heater: " + key);
		BOOST_CHECK_MESSAGE(key.rfind("chlorinator_", 0) != 0, "no chlorinator component should exist for an unlabelled chlorinator: " + key);
	}
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// AdoptRetainedComponents error/guard branches: an empty payload, a payload
// with no "cmps" object, and an unparseable payload must all be no-ops that
// adopt nothing (so the next publish tombstones nothing spuriously).
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HaDiscovery_AdoptRetained)

BOOST_AUTO_TEST_CASE(Test_AdoptRetained_EmptyPayload_AdoptsNothing)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	// Empty retained payload: early return, nothing adopted. The subsequent publish therefore
	// contains no tombstone for any device-slug component (there is nothing to tombstone).
	ha.AdoptRetainedComponents("");

	ha.PublishDiscoveryConfigs();
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	auto cmps = nlohmann::json::parse(queue.back().payload)["cmps"];
	BOOST_CHECK(!cmps.contains("aux_ghost"));
}

BOOST_AUTO_TEST_CASE(Test_AdoptRetained_NoCmpsObject_AdoptsNothing)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	// Valid JSON but no "cmps" key: guarded early return, nothing adopted.
	BOOST_CHECK_NO_THROW(ha.AdoptRetainedComponents(R"({"other":123})"));

	ha.PublishDiscoveryConfigs();
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	auto cmps = nlohmann::json::parse(queue.back().payload)["cmps"];
	BOOST_CHECK(!cmps.contains("aux_ghost"));
}

BOOST_AUTO_TEST_CASE(Test_AdoptRetained_InvalidJson_IsHarmless)
{
	boost::asio::io_context ioc;
	auto settings = MakeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	// Unparseable payload: the parse throws and is caught; the call is a no-op that does not escape.
	BOOST_CHECK_NO_THROW(ha.AdoptRetainedComponents("{ not valid json"));
}

BOOST_AUTO_TEST_SUITE_END()
