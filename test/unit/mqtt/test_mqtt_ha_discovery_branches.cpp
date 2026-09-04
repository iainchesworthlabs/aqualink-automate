#include <boost/test/unit_test.hpp>

#include <memory>
#include <optional>
#include <string>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/pool_configurations.h"
#include "kernel/preferences_hub.h"
#include "mqtt/ha_discovery.h"
#include "mqtt/mqtt_client.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

//=============================================================================
// HomeAssistantDiscovery branch tests.
//
// These complement test_mqtt_ha_discovery.cpp with the component-builder and
// retained-adoption arms it leaves unexercised: a connected preferences hub
// that is set to Celsius, the DualBody_DualEquipment / SingleBody circulation
// gates, the per-body chlorinator output numbers and remaining sensors, a
// retained config whose "cmps" is not an object or holds malformed components,
// and a device state that persists across two publish sweeps (no clear).
//=============================================================================

namespace
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	Options::Mqtt::MqttSettings MakeDiscoverySettings()
	{
		auto s = Test::MakeMqttSettings();
		s.topic_prefix = "aqualink";
		s.home_assistant_enabled = true;
		s.ha_discovery_prefix = "homeassistant";
		s.ha_device_id = "aqualink_branch";
		return s;
	}

	constexpr const char* CONFIG_TOPIC = "homeassistant/device/aqualink_branch/config";

	std::shared_ptr<Kernel::AuxillaryDevice> MakeTypedDevice(Traits::AuxillaryTypes type, const std::string& label)
	{
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		return dev;
	}

	// The "cmps" object of the LAST discovery config queued on the client.
	// Templated so the client's private PendingPublish type is never named.
	template <typename QueueT>
	nlohmann::json LastConfigComponents(const QueueT& queue)
	{
		nlohmann::json cmps;
		for (const auto& pending : queue)
		{
			if (pending.topic == CONFIG_TOPIC)
			{
				cmps = nlohmann::json::parse(pending.payload)["cmps"];
			}
		}
		return cmps;
	}

	template <typename QueueT>
	std::size_t CountOnTopic(const QueueT& queue, const std::string& topic)
	{
		std::size_t n = 0;
		for (const auto& pending : queue)
		{
			if (pending.topic == topic) { ++n; }
		}
		return n;
	}

	template <typename QueueT>
	bool AnyEmptyRetainedOnTopic(const QueueT& queue, const std::string& topic)
	{
		for (const auto& pending : queue)
		{
			if (pending.topic == topic && pending.payload.empty() && pending.retain) { return true; }
		}
		return false;
	}

	struct DiscoveryFixture
	{
		boost::asio::io_context ioc;
		Options::Mqtt::MqttSettings settings{ MakeDiscoverySettings() };
		std::shared_ptr<Mqtt::MqttClient> client{ std::make_shared<Mqtt::MqttClient>(ioc, settings) };
		Mqtt::HomeAssistantDiscovery ha{ client, settings };
		std::shared_ptr<Kernel::DataHub> data_hub{ std::make_shared<Kernel::DataHub>() };

		DiscoveryFixture()
		{
			ha.ConnectDataHub(data_hub);
		}

		auto& Queue() const
		{
			return Test::MqttClientPacketTest::GetPublishQueue(*client);
		}
	};
}

//=============================================================================
// Setpoint number entities: connected preferences hub set to Celsius
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HaDiscoveryBranches_SetpointUnits, DiscoveryFixture)

BOOST_AUTO_TEST_CASE(Test_SetpointUnits_ConnectedPrefsCelsius_DeclaresCelsiusRange)
{
	auto prefs = std::make_shared<Kernel::PreferencesHub>();
	prefs->Temperature_DisplayUnits = Kernel::TemperatureUnits::Celsius;
	ha.ConnectPreferencesHub(prefs);

	ha.PublishDiscoveryConfigs();

	auto cmps = LastConfigComponents(Queue());
	BOOST_REQUIRE(cmps.contains("pool_setpoint"));
	const auto& sp = cmps["pool_setpoint"];
	BOOST_CHECK_EQUAL(sp["p"], "number");
	BOOST_CHECK_EQUAL(sp["unit_of_measurement"], "°C");
	BOOST_CHECK_EQUAL(sp["min"].get<double>(), 15.0);
	BOOST_CHECK_EQUAL(sp["max"].get<double>(), 41.0);
	BOOST_CHECK_EQUAL(sp["step"].get<double>(), 0.5);
	BOOST_CHECK(sp["value_template"].get<std::string>().find(".celsius") != std::string::npos);
	BOOST_CHECK_EQUAL(sp["command_topic"], "aqualink/command/setpoint/pool");

	// Spa mirrors the same declared unit/range.
	BOOST_REQUIRE(cmps.contains("spa_setpoint"));
	BOOST_CHECK_EQUAL(cmps["spa_setpoint"]["unit_of_measurement"], "°C");
	BOOST_CHECK_EQUAL(cmps["spa_setpoint"]["command_topic"], "aqualink/command/setpoint/spa");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Circulation components: dual-body (dual equipment) vs single-body gating
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HaDiscoveryBranches_Circulation, DiscoveryFixture)

BOOST_AUTO_TEST_CASE(Test_Circulation_DualBodyDualEquipment_EmitsSpaModeSwitchAndSelect)
{
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_DualEquipment, Kernel::ConfigurationSource::UserSpecified);

	ha.PublishDiscoveryConfigs();

	auto cmps = LastConfigComponents(Queue());
	BOOST_REQUIRE(cmps.contains("spa_mode_switch"));
	const auto& sw = cmps["spa_mode_switch"];
	BOOST_CHECK_EQUAL(sw["p"], "switch");
	BOOST_CHECK_EQUAL(sw["command_topic"], "aqualink/command/circulation/mode");
	BOOST_CHECK_EQUAL(sw["payload_on"], "spa");
	BOOST_CHECK_EQUAL(sw["payload_off"], "pool");
	BOOST_CHECK_EQUAL(sw["state_topic"], "aqualink/pool/circulation");

	BOOST_REQUIRE(cmps.contains("circulation_mode_select"));
	const auto& sel = cmps["circulation_mode_select"];
	BOOST_CHECK_EQUAL(sel["p"], "select");
	BOOST_CHECK_EQUAL(sel["command_topic"], "aqualink/command/circulation/mode");
	BOOST_REQUIRE(sel["options"].is_array());
	BOOST_CHECK_EQUAL(sel["options"].size(), 3u);
	BOOST_CHECK_EQUAL(sel["options"][0], "Pool");
	BOOST_CHECK_EQUAL(sel["options"][1], "Spa");
	BOOST_CHECK_EQUAL(sel["options"][2], "Spillover");

	// The read-only mode sensor + the equipment-mode sensor are always present.
	BOOST_CHECK(cmps.contains("circulation_mode"));
	BOOST_CHECK_EQUAL(cmps["equipment_mode"]["state_topic"], "aqualink/pool/configuration");
}

BOOST_AUTO_TEST_CASE(Test_Circulation_SingleBody_OmitsWritableSpaModeEntities)
{
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::SingleBody, Kernel::ConfigurationSource::UserSpecified);

	ha.PublishDiscoveryConfigs();

	auto cmps = LastConfigComponents(Queue());
	BOOST_CHECK(!cmps.contains("spa_mode_switch"));
	BOOST_CHECK(!cmps.contains("circulation_mode_select"));
	// The read-only binary sensors stay regardless of the body layout.
	BOOST_CHECK(cmps.contains("spa_mode"));
	BOOST_CHECK(cmps.contains("clean_mode"));
	// Pool-only: the spa temperature/setpoint entities are gated off.
	BOOST_CHECK(cmps.contains("pool_setpoint"));
	BOOST_CHECK(!cmps.contains("spa_setpoint"));
	BOOST_CHECK(!cmps.contains("spa_temp"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Chlorinator components: per-body output numbers + every read-only sensor
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HaDiscoveryBranches_Chlorinator, DiscoveryFixture)

BOOST_AUTO_TEST_CASE(Test_ChlorinatorComponents_EmitsPerBodyOutputNumbers)
{
	data_hub->Devices.Add(MakeTypedDevice(Traits::AuxillaryTypes::Chlorinator, "Salt Cell"));

	ha.PublishDiscoveryConfigs();

	auto cmps = LastConfigComponents(Queue());

	// Pool output keeps the legacy key (unique_id continuity) and drives the pool topic.
	BOOST_REQUIRE(cmps.contains("chlorinator_salt_cell_pct_cmd"));
	const auto& pool_pct = cmps["chlorinator_salt_cell_pct_cmd"];
	BOOST_CHECK_EQUAL(pool_pct["p"], "number");
	BOOST_CHECK_EQUAL(pool_pct["name"], "Salt Cell Pool Output");
	BOOST_CHECK_EQUAL(pool_pct["value_template"], "{{ value_json.pool_setpoint_percent }}");
	BOOST_CHECK_EQUAL(pool_pct["command_topic"], "aqualink/command/chlorinator/percentage");
	BOOST_CHECK_EQUAL(pool_pct["state_topic"], "aqualink/device/salt_cell");

	// Spa output is an independent number on its own command topic.
	BOOST_REQUIRE(cmps.contains("chlorinator_salt_cell_spa_pct_cmd"));
	const auto& spa_pct = cmps["chlorinator_salt_cell_spa_pct_cmd"];
	BOOST_CHECK_EQUAL(spa_pct["p"], "number");
	BOOST_CHECK_EQUAL(spa_pct["name"], "Salt Cell Spa Output");
	BOOST_CHECK_EQUAL(spa_pct["value_template"], "{{ value_json.spa_setpoint_percent }}");
	BOOST_CHECK_EQUAL(spa_pct["command_topic"], "aqualink/command/chlorinator/spa/percentage");
	BOOST_CHECK_EQUAL(spa_pct["min"].get<int>(), 0);
	BOOST_CHECK_EQUAL(spa_pct["max"].get<int>(), 100);
	BOOST_CHECK_EQUAL(spa_pct["step"].get<int>(), 1);
	BOOST_CHECK_EQUAL(spa_pct["unit_of_measurement"], "%");
	BOOST_CHECK_EQUAL(spa_pct["mode"], "slider");
	BOOST_CHECK_EQUAL(spa_pct["unique_id"], "aqualink_branch_chlorinator_salt_cell_spa_pct_cmd");

	// The target-% sensor is a measurement with a unit; the output-state reason is not.
	BOOST_REQUIRE(cmps.contains("chlorinator_salt_cell_setpoint"));
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_setpoint"]["name"], "Salt Cell Target %");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_setpoint"]["unit_of_measurement"], "%");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_setpoint"]["state_class"], "measurement");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_setpoint"]["value_template"], "{{ value_json.setpoint_percent }}");

	BOOST_REQUIRE(cmps.contains("chlorinator_salt_cell_reason"));
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_reason"]["name"], "Salt Cell Output State");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell_reason"]["value_template"], "{{ value_json.generating_reason }}");
	BOOST_CHECK(!cmps["chlorinator_salt_cell_reason"].contains("unit_of_measurement"));
	BOOST_CHECK(!cmps["chlorinator_salt_cell_reason"].contains("state_class"));

	// The on/off switch for the chlorinator itself sits alongside its rich entities.
	BOOST_REQUIRE(cmps.contains("chlorinator_salt_cell"));
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell"]["p"], "switch");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell"]["state_topic"], "aqualink/ha/chlorinator_salt_cell");
	BOOST_CHECK_EQUAL(cmps["chlorinator_salt_cell"]["command_topic"], "aqualink/command/device/salt_cell");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Retained-config adoption: malformed "cmps" shapes
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HaDiscoveryBranches_AdoptRetained, DiscoveryFixture)

BOOST_AUTO_TEST_CASE(Test_AdoptRetained_CmpsNotAnObject_AdoptsNothing)
{
	ha.AdoptRetainedComponents(R"({"cmps":[{"p":"switch"},{"p":"sensor"}]})");

	ha.PublishDiscoveryConfigs();

	// Nothing was adopted, so no tombstone ({"p": platform} only) appears in the fresh config.
	auto cmps = LastConfigComponents(Queue());
	BOOST_REQUIRE(cmps.is_object());
	for (const auto& [key, component] : cmps.items())
	{
		BOOST_CHECK_MESSAGE(component.size() > 1, "unexpected tombstone for component: " + key);
	}
}

BOOST_AUTO_TEST_CASE(Test_AdoptRetained_MalformedComponents_SkippedWellFormedTombstoned)
{
	nlohmann::json retained;
	retained["cmps"]["ghost_number"] = 5;                                        // not an object
	retained["cmps"]["ghost_empty"] = nlohmann::json::object();                  // no platform
	retained["cmps"]["ghost_bad_platform"] = { {"p", 7} };                       // platform not a string
	retained["cmps"]["ghost_ok"] = { {"p", "switch"}, {"name", "Old Light"} };   // adoptable
	ha.AdoptRetainedComponents(retained.dump());

	ha.PublishDiscoveryConfigs();

	auto cmps = LastConfigComponents(Queue());
	BOOST_REQUIRE(cmps.contains("ghost_ok"));
	BOOST_CHECK_EQUAL(cmps["ghost_ok"].size(), 1u);   // tombstoned: platform only
	BOOST_CHECK_EQUAL(cmps["ghost_ok"]["p"], "switch");
	BOOST_CHECK(!cmps.contains("ghost_number"));
	BOOST_CHECK(!cmps.contains("ghost_empty"));
	BOOST_CHECK(!cmps.contains("ghost_bad_platform"));

	// A tombstone is emitted exactly once: the next cycle no longer tracks it.
	ha.PublishDiscoveryConfigs();
	BOOST_CHECK(!LastConfigComponents(Queue()).contains("ghost_ok"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Device states: a device that persists across sweeps is never cleared
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HaDiscoveryBranches_DeviceStates, DiscoveryFixture)

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_UnchangedDevice_RepublishedNotCleared)
{
	auto pump = MakeTypedDevice(Traits::AuxillaryTypes::Pump, "Filter Pump");
	pump->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);
	data_hub->Devices.Add(pump);

	ha.PublishDeviceStates();
	ha.PublishDeviceStates();

	const std::string state_topic = "aqualink/ha/pump_filter_pump";
	BOOST_CHECK_EQUAL(CountOnTopic(Queue(), state_topic), 2u);
	BOOST_CHECK(!AnyEmptyRetainedOnTopic(Queue(), state_topic));
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStates_RelabelledDevice_ClearsOldTopicOnly)
{
	auto aux = MakeTypedDevice(Traits::AuxillaryTypes::Auxillary, "Old Name");
	data_hub->Devices.Add(aux);
	ha.PublishDeviceStates();

	aux->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "New Name" });
	ha.PublishDeviceStates();

	BOOST_CHECK(AnyEmptyRetainedOnTopic(Queue(), "aqualink/ha/aux_old_name"));
	BOOST_CHECK_EQUAL(CountOnTopic(Queue(), "aqualink/ha/aux_new_name"), 1u);
	BOOST_CHECK(!AnyEmptyRetainedOnTopic(Queue(), "aqualink/ha/aux_new_name"));
}

BOOST_AUTO_TEST_SUITE_END()
