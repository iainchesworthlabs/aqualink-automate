#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include "mqtt/ha_discovery.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_topic_scheme.h"
#include "kernel/data_hub.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

namespace
{
	Options::Mqtt::MqttSettings MakeSchemeTestSettings()
	{
		auto s = Test::MakeMqttSettings();
		s.topic_prefix = "aqualink";
		s.home_assistant_enabled = true;
		s.ha_discovery_prefix = "homeassistant";
		s.ha_device_id = "aqualink_aqualink";
		return s;
	}

	std::shared_ptr<Kernel::AuxillaryDevice> MakeDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes type, const std::string& label)
	{
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, label);
		return dev;
	}

	// Drive a hub that is forced into the connected state and connected to a real
	// DataHub. Returns the connected client so the test can inspect its publish queue.
	std::shared_ptr<Mqtt::MqttClient> StartConnectedHub(Mqtt::MqttHub& hub, const std::shared_ptr<Kernel::DataHub>& data_hub)
	{
		hub.ConnectDataHub(data_hub);
		hub.Start();
		auto client = hub.GetMqttClient();
		Test::MqttClientPacketTest::ForceConnectedState(*client);
		return client;
	}
}

//=============================================================================
// TopicScheme primitives
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_Mqtt_TopicScheme)

BOOST_AUTO_TEST_CASE(Test_CategoryName_CanonicalSpellings)
{
	using Mqtt::TopicScheme::CategoryName;
	using Mqtt::TopicScheme::DeviceCategory;

	BOOST_CHECK_EQUAL(std::string(CategoryName(DeviceCategory::Auxillary)), "aux");
	BOOST_CHECK_EQUAL(std::string(CategoryName(DeviceCategory::Heater)), "heater");
	BOOST_CHECK_EQUAL(std::string(CategoryName(DeviceCategory::Pump)), "pump");
	BOOST_CHECK_EQUAL(std::string(CategoryName(DeviceCategory::Chlorinator)), "chlorinator");
	BOOST_CHECK_EQUAL(std::string(CategoryName(DeviceCategory::Light)), "light");
}

BOOST_AUTO_TEST_CASE(Test_DeviceJsonSubtopic_Format)
{
	BOOST_CHECK_EQUAL(Mqtt::TopicScheme::DeviceJsonSubtopic("filter_pump"), "device/filter_pump");
}

BOOST_AUTO_TEST_CASE(Test_DeviceStateSubtopic_Format)
{
	using Mqtt::TopicScheme::DeviceCategory;
	using Mqtt::TopicScheme::DeviceStateSubtopic;

	BOOST_CHECK_EQUAL(DeviceStateSubtopic(DeviceCategory::Pump, "filter_pump"), "ha/pump_filter_pump");
	BOOST_CHECK_EQUAL(DeviceStateSubtopic(DeviceCategory::Auxillary, "spa_light"), "ha/aux_spa_light");
	BOOST_CHECK_EQUAL(DeviceStateSubtopic(DeviceCategory::Light, "light_0xf0"), "ha/light_light_0xf0");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Contract: discovery state_topic == the topic that the publisher writes to.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_Mqtt_TopicScheme_Contract)

// A pump's HA switch state_topic must equal the short-string topic the HA state
// publisher (and, equivalently, TopicScheme::DeviceStateSubtopic) writes to.
BOOST_AUTO_TEST_CASE(Test_PumpSwitchStateTopic_MatchesDeviceStatePublisher)
{
	boost::asio::io_context ioc;
	auto settings = MakeSchemeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	auto pump = MakeDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Pump, "Filter Pump");
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Off);
	data_hub->Devices.Add(pump);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	const auto expected_state_topic = client->BuildTopic(
		Mqtt::TopicScheme::DeviceStateSubtopic(Mqtt::TopicScheme::DeviceCategory::Pump, "filter_pump"));

	BOOST_REQUIRE(cmps.contains("pump_filter_pump"));
	BOOST_CHECK_EQUAL(cmps["pump_filter_pump"]["state_topic"].get<std::string>(), expected_state_topic);
	BOOST_CHECK_EQUAL(expected_state_topic, "aqualink/ha/pump_filter_pump");
}

// A chlorinator's extra (value_json) entities must read the JSON-blob topic that
// MqttHub::PublishDeviceStatus writes to (TopicScheme::DeviceJsonSubtopic).
BOOST_AUTO_TEST_CASE(Test_ChlorinatorSensorStateTopic_MatchesJsonPublisher)
{
	boost::asio::io_context ioc;
	auto settings = MakeSchemeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	auto chlorinator = MakeDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Chlorinator, "AquaPure");
	chlorinator->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
	data_hub->Devices.Add(chlorinator);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	const auto expected_json_topic = client->BuildTopic(Mqtt::TopicScheme::DeviceJsonSubtopic("aquapure"));

	BOOST_REQUIRE(cmps.contains("chlorinator_aquapure_generating"));
	BOOST_CHECK_EQUAL(cmps["chlorinator_aquapure_generating"]["state_topic"].get<std::string>(), expected_json_topic);
	BOOST_CHECK_EQUAL(expected_json_topic, "aqualink/device/aquapure");

	// The chlorinator's primary ON/OFF switch, by contrast, reads the short-string topic.
	BOOST_REQUIRE(cmps.contains("chlorinator_aquapure"));
	BOOST_CHECK_EQUAL(cmps["chlorinator_aquapure"]["state_topic"].get<std::string>(), "aqualink/ha/chlorinator_aquapure");
}

// Reconciliation: the auxiliary category is spelled "aux" everywhere now (the hub
// previously tagged it "auxillary" while discovery used "aux").
BOOST_AUTO_TEST_CASE(Test_AuxiliaryCategory_ReconciledToAux)
{
	boost::asio::io_context ioc;
	auto settings = MakeSchemeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	auto aux = MakeDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Auxillary, "Pool Light");
	data_hub->Devices.Add(aux);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	BOOST_REQUIRE(cmps.contains("aux_pool_light"));
	BOOST_CHECK_EQUAL(cmps["aux_pool_light"]["state_topic"].get<std::string>(), "aqualink/ha/aux_pool_light");
}

//-----------------------------------------------------------------------------
// Lights are published READ-ONLY.
//
// A light is a separate RS-485 colour-light controller (bus ids 0xF0-0xF4), not the
// aux relay that switches it. It carries no JandyAuxillaryId and only a synthetic
// label, so no controller can map an actuation to it -- every path returns
// MappingFailed -> UnknownEquipmentType. A switch entity would therefore be a
// permanently-broken duplicate of the aux switch that actually drives the light.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_Light_PublishesReadOnlyEntitiesAndNoCommandTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeSchemeTestSettings();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	Mqtt::HomeAssistantDiscovery ha(client, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	ha.ConnectDataHub(data_hub);

	auto light = MakeDevice(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Light, "Light 0xf0");
	light->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);
	light->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ColourTrait{}, static_cast<uint8_t>(4));
	data_hub->Devices.Add(light);

	ha.PublishDiscoveryConfigs();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	BOOST_REQUIRE_EQUAL(queue.size(), 1);

	auto payload = nlohmann::json::parse(queue[0].payload);
	auto& cmps = payload["cmps"];

	// The raw multi-state sensor, on the short-string state topic the publisher writes.
	BOOST_REQUIRE(cmps.contains("light_light_0xf0"));
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0"]["p"].get<std::string>(), "sensor");
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0"]["state_topic"].get<std::string>(), "aqualink/ha/light_light_0xf0");

	// The derived on/off binary sensor.
	BOOST_REQUIRE(cmps.contains("light_light_0xf0_on"));
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0_on"]["p"].get<std::string>(), "binary_sensor");
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0_on"]["state_topic"].get<std::string>(), "aqualink/ha/light_light_0xf0");

	// The colour/mode sensor, reading the JSON blob topic.
	BOOST_REQUIRE(cmps.contains("light_light_0xf0_mode"));
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0_mode"]["p"].get<std::string>(), "sensor");
	BOOST_CHECK_EQUAL(cmps["light_light_0xf0_mode"]["state_topic"].get<std::string>(), "aqualink/device/light_0xf0");

	// The contract that matters: NOTHING published for a light may be writable.
	for (const auto& [key, component] : cmps.items())
	{
		if (key.starts_with("light_"))
		{
			BOOST_CHECK_MESSAGE(!component.contains("command_topic"),
				"Light component '" << key << "' must not be writable - a light cannot be actuated");
			const auto platform = component.value("p", std::string{});
			BOOST_CHECK_MESSAGE(platform == "sensor" || platform == "binary_sensor",
				"Light component '" << key << "' has writable platform '" << platform << "'");
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// publish_on_change: debounced on-change publishing via the hub-change signals.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_PublishOnChange)

// A config change must NOT publish synchronously from the (hot-path) signal handler:
// publishing is deferred to Poll() and debounced. Asserting the queue is unchanged
// immediately after a burst of changes pins the "no inline publish on the decode hot
// path" design regardless of broker/socket state.
BOOST_AUTO_TEST_CASE(Test_OnChange_EnabledDoesNotPublishInline)
{
	boost::asio::io_context ioc;
	auto settings = Test::MakeMqttSettings();
	settings.publish_on_change = true;
	// Push the periodic timers far out so they cannot interfere with this test.
	settings.status_publish_interval = std::chrono::seconds(3600);
	settings.statistics_publish_interval = std::chrono::seconds(3600);

	Mqtt::MqttHub hub(ioc, settings);
	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto client = StartConnectedHub(hub, data_hub);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	auto baseline = queue.size();

	// Fire a burst of config changes; the handler must only ARM a deferred publish,
	// never publish inline.
	data_hub->AirTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(25.0));
	data_hub->PoolTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(26.0));
	data_hub->SpaTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(38.0));

	BOOST_CHECK_EQUAL(queue.size(), baseline);

	hub.Stop();
}

// When publish_on_change is disabled, a config change is a complete no-op (it neither
// publishes inline nor arms a deferred publish).
BOOST_AUTO_TEST_CASE(Test_OnChange_DisabledIsInert)
{
	boost::asio::io_context ioc;
	auto settings = Test::MakeMqttSettings();
	settings.publish_on_change = false;
	settings.status_publish_interval = std::chrono::seconds(3600);
	settings.statistics_publish_interval = std::chrono::seconds(3600);

	Mqtt::MqttHub hub(ioc, settings);
	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto client = StartConnectedHub(hub, data_hub);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*client);
	auto baseline = queue.size();

	BOOST_CHECK_NO_THROW(data_hub->AirTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(25.0)));

	BOOST_CHECK_EQUAL(queue.size(), baseline);

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
