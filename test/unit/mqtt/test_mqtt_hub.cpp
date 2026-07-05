#include <boost/test/unit_test.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_client.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/statistics_hub.h"
#include "kernel/temperature.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

namespace
{
	Options::Mqtt::MqttSettings MakeHubTestSettings()
	{
		return Test::MakeMqttSettings();
	}

	Kernel::Temperature Celsius(double c)
	{
		return Kernel::Temperature::ConvertToTemperatureInCelsius(c);
	}

	// PublishAllStatus() only emits when IsRunning() (hub started AND client connected).
	// Start the hub then force the client's connected state so the publish paths run and
	// enqueue against the offline client, then hand back the queue for inspection. Returns
	// by reference via auto& so the private PendingPublish element type is never named.
	auto& PublishAllAndGetQueue(Mqtt::MqttHub& hub)
	{
		hub.Start();
		Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
		hub.PublishAllStatus();
		return Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	}

	// First published payload whose topic contains `needle` (parsed as JSON), or nullopt.
	// Templated on the queue so the private PendingPublish element type is never named.
	template <typename QueueT>
	std::optional<nlohmann::json> FindPayloadContaining(const QueueT& queue, const std::string& needle)
	{
		for (const auto& pending : queue)
		{
			if (pending.topic.find(needle) != std::string::npos)
			{
				return nlohmann::json::parse(pending.payload);
			}
		}
		return std::nullopt;
	}
}

//=============================================================================
// MqttHub construction tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_Construction)

BOOST_AUTO_TEST_CASE(Test_Construction_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();

	BOOST_CHECK_NO_THROW(Mqtt::MqttHub hub(ioc, settings));
}

BOOST_AUTO_TEST_CASE(Test_Construction_NotRunning)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK(!hub.IsRunning());
}

BOOST_AUTO_TEST_CASE(Test_Construction_ClientCreated)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK(hub.GetMqttClient() != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub lifecycle tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_Lifecycle)

BOOST_AUTO_TEST_CASE(Test_Start_SetsRunningFlag)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.Start();

	// IsRunning() will be false since the client is not connected to a broker
	BOOST_CHECK(!hub.IsRunning());

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_Start_TwiceIsHarmless)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.Start();
	BOOST_CHECK_NO_THROW(hub.Start()); // Should not throw

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_Stop_ClearsRunningFlag)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.Start();
	hub.Stop();

	BOOST_CHECK(!hub.IsRunning());
}

BOOST_AUTO_TEST_CASE(Test_Stop_WhenNotRunningIsHarmless)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_NO_THROW(hub.Stop());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub hub connections tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_HubConnections)

BOOST_AUTO_TEST_CASE(Test_ConnectDataHub_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();

	BOOST_CHECK_NO_THROW(hub.ConnectDataHub(data_hub));
}

BOOST_AUTO_TEST_CASE(Test_ConnectEquipmentHub_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();

	BOOST_CHECK_NO_THROW(hub.ConnectEquipmentHub(equip_hub));
}

BOOST_AUTO_TEST_CASE(Test_ConnectStatisticsHub_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	BOOST_CHECK_NO_THROW(hub.ConnectStatisticsHub(stats_hub));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub command handler tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_CommandHandlers)

BOOST_AUTO_TEST_CASE(Test_RegisterCommand_AddsHandler)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.RegisterCommand("test_cmd", [](const std::string&, const nlohmann::json&) {});

	BOOST_CHECK_EQUAL(hub.CommandCount(), 1);
	BOOST_CHECK(hub.HasCommand("test_cmd"));
}

BOOST_AUTO_TEST_CASE(Test_RegisterCommand_MultipleHandlers)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.RegisterCommand("cmd1", [](const std::string&, const nlohmann::json&) {});
	hub.RegisterCommand("cmd2", [](const std::string&, const nlohmann::json&) {});
	hub.RegisterCommand("cmd3", [](const std::string&, const nlohmann::json&) {});

	BOOST_CHECK_EQUAL(hub.CommandCount(), 3);
}

BOOST_AUTO_TEST_CASE(Test_UnregisterCommand_RemovesHandler)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	hub.RegisterCommand("to_remove", [](const std::string&, const nlohmann::json&) {});
	BOOST_CHECK_EQUAL(hub.CommandCount(), 1);

	hub.UnregisterCommand("to_remove");
	BOOST_CHECK_EQUAL(hub.CommandCount(), 0);
}

BOOST_AUTO_TEST_CASE(Test_UnregisterCommand_NonexistentIsHarmless)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_NO_THROW(hub.UnregisterCommand("nonexistent"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub topic building tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_Topics)

BOOST_AUTO_TEST_CASE(Test_StatusTopic_Format)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_EQUAL(hub.StatusTopic("pool"), "test/status/pool");
	BOOST_CHECK_EQUAL(hub.StatusTopic("system"), "test/status/system");
	BOOST_CHECK_EQUAL(hub.StatusTopic("devices"), "test/status/devices");
}

BOOST_AUTO_TEST_CASE(Test_EventTopic_Format)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_EQUAL(hub.EventTopic("config_change"), "test/event/config_change");
}

BOOST_AUTO_TEST_CASE(Test_CommandTopic_Format)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_EQUAL(hub.CommandTopic("status"), "test/command/status");
	BOOST_CHECK_EQUAL(hub.CommandTopic("device"), "test/command/device");
}

BOOST_AUTO_TEST_CASE(Test_IsCommandTopic_MatchesCommandPrefix)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK(hub.IsCommandTopic("test/command/status"));
	BOOST_CHECK(hub.IsCommandTopic("test/command/device"));
	BOOST_CHECK(!hub.IsCommandTopic("test/status/pool"));
	BOOST_CHECK(!hub.IsCommandTopic("other/command/test"));
}

BOOST_AUTO_TEST_CASE(Test_IsCommandTopic_DoesNotMatchWithoutTrailingSlash)
{
	// Regression: "command_extra" must not match as a command topic
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK(!hub.IsCommandTopic("test/command_extra"));
	BOOST_CHECK(!hub.IsCommandTopic("test/commandsuffix"));
}

BOOST_AUTO_TEST_CASE(Test_ExtractCommand_ExtractsAction)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_EQUAL(hub.ExtractCommand("test/command/status"), "status");
	BOOST_CHECK_EQUAL(hub.ExtractCommand("test/command/device"), "device");
	BOOST_CHECK_EQUAL(hub.ExtractCommand("test/command/temperature"), "temperature");
}

BOOST_AUTO_TEST_CASE(Test_ExtractCommand_NonCommandTopicReturnsEmpty)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_EQUAL(hub.ExtractCommand("test/status/pool"), "");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub signal tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_Signals)

BOOST_AUTO_TEST_CASE(Test_OnDevicesPublished_SignalExists)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	bool signal_fired = false;
	auto connection = hub.OnDevicesPublished.connect([&signal_fired]() {
		signal_fired = true;
	});

	// We can't trigger PublishDeviceStatus without being connected,
	// but we can verify the signal connection doesn't throw
	BOOST_CHECK(!signal_fired);
}

BOOST_AUTO_TEST_CASE(Test_GetMqttClient_ReturnsSharedClient)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto client1 = hub.GetMqttClient();
	auto client2 = hub.GetMqttClient();

	BOOST_CHECK(client1 != nullptr);
	BOOST_CHECK_EQUAL(client1.get(), client2.get()); // Same pointer
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub no-crash tests (publish methods when not connected)
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_NoCrash)

BOOST_AUTO_TEST_CASE(Test_PublishAllStatus_WhenNotRunning_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_NO_THROW(hub.PublishAllStatus());
}

BOOST_AUTO_TEST_CASE(Test_PublishCustom_WhenNotRunning_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	nlohmann::json payload = {{"test", "value"}};
	BOOST_CHECK_NO_THROW(hub.PublishCustom("custom/test", payload));
}

BOOST_AUTO_TEST_CASE(Test_Poll_WhenNotRunning_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	BOOST_CHECK_NO_THROW(hub.Poll());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttHub temperature freshness tests
//
// Each published temperature now carries freshness metadata (last_updated +
// stale) so a Home Assistant consumer can tell a reading has gone old without
// the value disappearing. These tests drive PublishAllStatus() and assert the
// serialized shape on the pool/temperatures and per-body topics.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_TemperatureFreshness)

BOOST_AUTO_TEST_CASE(Test_PoolTemperatures_CarryFreshnessMetadata)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	// Setting a temperature stamps its UpdatedAt; a value just set is fresh (not stale).
	data_hub->AirTemp(Celsius(24.0));
	data_hub->PoolTemp(Celsius(26.0));
	data_hub->SpaTemp(Celsius(38.0));
	data_hub->PoolTempSetpoint(Celsius(28.0));
	data_hub->SpaTempSetpoint(Celsius(39.0));
	data_hub->FreezeProtectPoint(Celsius(2.0));
	hub.ConnectDataHub(data_hub);

	auto& queue = PublishAllAndGetQueue(hub);
	auto temps = FindPayloadContaining(queue, "/pool/temperatures");
	BOOST_REQUIRE_MESSAGE(temps.has_value(), "pool/temperatures was not published");

	// Each live temperature carries {celsius, last_updated, stale}; a freshly set value
	// has a populated (non-null) last_updated and is not stale.
	for (const char* key : { "air", "pool", "spa" })
	{
		BOOST_REQUIRE_MESSAGE(temps->contains(key), std::string("missing temperature key: ") + key);
		const auto& measurement = (*temps)[key];
		BOOST_CHECK_MESSAGE(measurement.contains("celsius"), std::string(key) + " missing celsius");
		BOOST_REQUIRE_MESSAGE(measurement.contains("last_updated"), std::string(key) + " missing last_updated");
		BOOST_REQUIRE_MESSAGE(measurement.contains("stale"), std::string(key) + " missing stale");
		BOOST_CHECK_MESSAGE(!measurement["last_updated"].is_null(), std::string(key) + " last_updated should be populated");
		BOOST_CHECK_EQUAL(measurement["stale"].get<bool>(), false);
	}

	// Setpoints are configured values: they carry a timestamp but are never flagged stale.
	BOOST_REQUIRE(temps->contains("pool_setpoint"));
	BOOST_CHECK_EQUAL((*temps)["pool_setpoint"]["stale"].get<bool>(), false);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_PerBodyTemperatures_PublishedWithFreshnessShape)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	// A dual-body (combo) system gives Pool + Spa bodies, so per-body temperature topics
	// are published (and the body-id freshness switch is exercised for both cases).
	data_hub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::UserSpecified);
	data_hub->PoolTemp(Celsius(26.0));
	data_hub->SpaTemp(Celsius(38.0));
	hub.ConnectDataHub(data_hub);

	auto& queue = PublishAllAndGetQueue(hub);
	auto body = FindPayloadContaining(queue, "/body/");
	BOOST_REQUIRE_MESSAGE(body.has_value(), "no per-body temperature topic was published");

	// Each body topic reports current + setpoint + is_active; current/setpoint go through
	// the freshness-aware serializer (current may be null for an inactive combo body, but
	// the keys are always present).
	BOOST_CHECK(body->contains("current"));
	BOOST_CHECK(body->contains("setpoint"));
	BOOST_CHECK(body->contains("is_active"));

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Inbound message routing (HandleMessage -> ProcessCommand). The hub subscribes
// to the client's OnMessageReceived in its constructor, so firing that signal
// drives the real parse + dispatch without a broker.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_MessageRouting)

BOOST_AUTO_TEST_CASE(Test_HandleMessage_ValidJson_RoutedToHandler)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);
	hub.Start();   // wires the client OnMessageReceived -> HandleMessage connection

	std::optional<nlohmann::json> captured;
	hub.RegisterCommand("ctrl", [&](const std::string&, const nlohmann::json& p) { captured = p; });

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("ctrl"), R"({"v":42})");

	BOOST_REQUIRE(captured.has_value());
	BOOST_CHECK_EQUAL(captured->value("v", 0), 42);
}

BOOST_AUTO_TEST_CASE(Test_HandleMessage_InvalidJson_FallsBackToRaw)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);
	hub.Start();

	std::optional<nlohmann::json> captured;
	hub.RegisterCommand("ctrl", [&](const std::string&, const nlohmann::json& p) { captured = p; });

	// Unparseable payload: the handler still runs, with the raw text wrapped as {"raw": ...}.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("ctrl"), "{ not valid json");

	BOOST_REQUIRE(captured.has_value());
	BOOST_REQUIRE(captured->contains("raw"));
	BOOST_CHECK_EQUAL(captured->at("raw").get<std::string>(), "{ not valid json");
}

BOOST_AUTO_TEST_CASE(Test_HandleMessage_EmptyPayload_RoutedWithEmptyJson)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);
	hub.Start();

	bool called = false;
	std::optional<nlohmann::json> captured;
	hub.RegisterCommand("ctrl", [&](const std::string&, const nlohmann::json& p) { called = true; captured = p; });

	// Empty payload bypasses JSON parsing entirely; the handler still fires with a null json.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("ctrl"), "");

	BOOST_CHECK(called);
	BOOST_REQUIRE(captured.has_value());
	BOOST_CHECK(captured->is_null());
}

BOOST_AUTO_TEST_CASE(Test_HandleMessage_NonCommandTopic_Ignored)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);
	hub.Start();

	bool called = false;
	hub.RegisterCommand("ctrl", [&](const std::string&, const nlohmann::json&) { called = true; });

	// A status topic is not a command topic, so HandleMessage drops it before dispatch.
	hub.GetMqttClient()->OnMessageReceived(hub.StatusTopic("ctrl"), R"({"v":1})");

	BOOST_CHECK(!called);
}

BOOST_AUTO_TEST_CASE(Test_HandleMessage_UnknownCommand_IsHarmless)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);
	hub.Start();

	// No handler registered for "ghost": ProcessCommand logs and returns without throwing.
	BOOST_CHECK_NO_THROW(hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("ghost"), R"({"v":1})"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Retained-topic lifecycle: a device that drops out (removed, or relabelled so its slug
// changes) must have its previously-published retained topic cleared, so the broker stops
// serving a stale/duplicate device.
//=============================================================================

namespace
{
	// Payload+retain of the LAST queued publish whose topic ends with `suffix`, or nullopt.
	// Templated so the private PendingPublish element type is never named.
	template <typename QueueT>
	std::optional<std::pair<std::string, bool>> LastEntryForTopicEnding(const QueueT& queue, const std::string& suffix)
	{
		std::optional<std::pair<std::string, bool>> found;
		for (const auto& pending : queue)
		{
			if (pending.topic.size() >= suffix.size()
				&& 0 == pending.topic.compare(pending.topic.size() - suffix.size(), suffix.size(), suffix))
			{
				found = std::make_pair(pending.payload, pending.retain);
			}
		}
		return found;
	}

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

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_RetainedTopicLifecycle)

BOOST_AUTO_TEST_CASE(Test_RemovedDevice_ClearsRetainedDeviceTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto aux = MakeAuxOn("Aux5");
	data_hub->Devices.Add(aux);
	hub.ConnectDataHub(data_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// First publish emits the device JSON topic with a real payload.
	hub.PublishAllStatus();
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto first = LastEntryForTopicEnding(queue, "/device/aux5");
	BOOST_REQUIRE_MESSAGE(first.has_value(), "device/aux5 was not published");
	BOOST_CHECK(!first->first.empty());

	// Remove the device: the next sweep clears the now-vanished topic (empty + retained).
	data_hub->Devices.Remove(aux);
	hub.PublishAllStatus();

	auto cleared = LastEntryForTopicEnding(queue, "/device/aux5");
	BOOST_REQUIRE(cleared.has_value());
	BOOST_CHECK_MESSAGE(cleared->first.empty(), "vanished device topic should be cleared with an empty payload");
	BOOST_CHECK_MESSAGE(cleared->second, "the clearing publish must be retained");

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_RelabelledDevice_ClearsOldSlugTopic)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto aux = MakeAuxOn("Aux5");
	data_hub->Devices.Add(aux);
	hub.ConnectDataHub(data_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
	hub.PublishAllStatus();

	// The label is enumerated -> the slug changes from "aux5" to "pool_light".
	aux->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, std::string("Pool Light"));
	hub.PublishAllStatus();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());

	// New slug carries the live payload; the old slug topic is cleared.
	auto new_topic = LastEntryForTopicEnding(queue, "/device/pool_light");
	BOOST_REQUIRE(new_topic.has_value());
	BOOST_CHECK(!new_topic->first.empty());

	auto old_topic = LastEntryForTopicEnding(queue, "/device/aux5");
	BOOST_REQUIRE(old_topic.has_value());
	BOOST_CHECK_MESSAGE(old_topic->first.empty(), "the stale slug topic should be cleared");
	BOOST_CHECK(old_topic->second);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_StartupReconcile_ClearsUnownedRetainedTopics)
{
	// Startup broker reconciliation: a ghost retained topic left by a PRIOR (buggy) run - which the
	// per-sweep diff cannot see because it starts each process with an empty published-set - is
	// cleared by the post-connect sweep that subscribes, collects what the broker holds, and clears
	// anything the current device set no longer owns.
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	settings.home_assistant_enabled = true;   // so HA short-state topics are part of the owned set
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeAuxOn("Pool Light"));
	hub.ConnectDataHub(data_hub);

	// The broker is serving the live device's topics PLUS ghost topics from a prior run.
	const std::string owned_json  = "test/device/pool_light";
	const std::string owned_state = "test/ha/aux_pool_light";
	const std::string ghost_json  = "test/device/aux5";
	const std::string ghost_state = "test/ha/aux_aux5";
	Test::MqttHubReconcileTest::SeedSeenRetainedTopics(hub, { owned_json, owned_state, ghost_json, ghost_state });

	Test::MqttHubReconcileTest::CallReconcileRetainedTopics(hub);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto clear_of = [&](const std::string& topic) -> std::optional<std::pair<std::string, bool>>
	{
		for (const auto& pending : queue)
		{
			if (pending.topic == topic) { return std::make_pair(pending.payload, pending.retain); }
		}
		return std::nullopt;
	};

	// Ghost topics cleared (empty + retained); owned topics left untouched.
	auto gj = clear_of(ghost_json);
	BOOST_REQUIRE_MESSAGE(gj.has_value(), "ghost device topic should be cleared");
	BOOST_CHECK(gj->first.empty());
	BOOST_CHECK(gj->second);
	auto gs = clear_of(ghost_state);
	BOOST_REQUIRE_MESSAGE(gs.has_value(), "ghost HA state topic should be cleared");
	BOOST_CHECK(gs->first.empty());
	BOOST_CHECK(gs->second);
	BOOST_CHECK_MESSAGE(!clear_of(owned_json).has_value(), "owned device topic must not be cleared");
	BOOST_CHECK_MESSAGE(!clear_of(owned_state).has_value(), "owned HA state topic must not be cleared");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// OnConnected wiring: firing the client's OnConnected signal (as the real
// broker connect would) must republish the static topics and arm the startup
// retained-topic reconciliation window.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_OnConnected)

BOOST_AUTO_TEST_CASE(Test_OnConnected_RepublishesStaticTopicsAndArmsReconcile)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	hub.ConnectDataHub(data_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// Fire the client's OnConnected signal exactly as a real broker connect would. This runs the
	// hub's connect lambda: it subscribes to the command + device/HA wildcards, arms the retained
	// reconciliation window, and calls PublishAllStatus() (which publishes the static topics).
	hub.GetMqttClient()->OnConnected();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto version = FindPayloadContaining(queue, "/system/version");
	auto equipment = FindPayloadContaining(queue, "/system/equipment");
	BOOST_CHECK_MESSAGE(version.has_value(), "system/version should be (re)published on connect");
	BOOST_CHECK_MESSAGE(equipment.has_value(), "system/equipment should be (re)published on connect");

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_HandleMessage_DuringReconcileWindow_CollectsRetainedTopics)
{
	// While the startup reconciliation window is open (armed by OnConnected), a retained device or
	// HA-state topic replayed by the broker is recorded in m_SeenRetainedTopics. A subsequent
	// ReconcileRetainedTopics() then clears any collected topic the current device set does not own.
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	hub.ConnectDataHub(data_hub);   // no devices -> nothing is "owned"

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
	hub.GetMqttClient()->OnConnected();   // arms the reconcile window + records the device/ha prefixes

	// Broker replays a retained device topic with a non-empty payload while the window is open.
	hub.GetMqttClient()->OnMessageReceived("test/device/ghost", R"({"stale":true})");

	// The reconcile pass (driven directly via the test seam) must clear that collected topic, since
	// the empty device set owns nothing.
	Test::MqttHubReconcileTest::CallReconcileRetainedTopics(hub);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto cleared = LastEntryForTopicEnding(queue, "/device/ghost");
	BOOST_REQUIRE_MESSAGE(cleared.has_value(), "collected retained topic should have been cleared");
	BOOST_CHECK_MESSAGE(cleared->first.empty(), "the clearing publish should carry an empty payload");
	BOOST_CHECK(cleared->second);

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Device-status publishing across every device category (auxillary / heater /
// pump / chlorinator), the no-label skip path, and owned-topic computation.
//=============================================================================

namespace
{
	std::shared_ptr<Kernel::AuxillaryDevice> MakeDeviceOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes type, const std::string& label)
	{
		namespace Traits = Kernel::AuxillaryTraitsTypes;
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		return dev;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_DeviceStatusCategories)

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStatus_AllCategoriesPublished)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Auxillary, "Pool Light"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Heater, "Pool Heater"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Pump, "Filter Pump"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Chlorinator, "AquaPure"));
	hub.ConnectDataHub(data_hub);

	auto& queue = PublishAllAndGetQueue(hub);

	// Each category is published to its own device/{slug} JSON topic carrying its category type.
	struct Expected { const char* topic; const char* type; };
	for (const auto& e : { Expected{ "/device/pool_light", "aux" },
	                       Expected{ "/device/pool_heater", "heater" },
	                       Expected{ "/device/filter_pump", "pump" },
	                       Expected{ "/device/aquapure", "chlorinator" } })
	{
		auto payload = FindPayloadContaining(queue, e.topic);
		BOOST_REQUIRE_MESSAGE(payload.has_value(), std::string("missing device topic: ") + e.topic);
		BOOST_CHECK_EQUAL((*payload)["type"].get<std::string>(), e.type);
	}

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStatus_DeviceWithoutLabel_Skipped)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	// An auxillary with NO label trait: the publish lambda skips it (no slug can be formed).
	auto unlabelled = std::make_shared<Kernel::AuxillaryDevice>();
	unlabelled->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(unlabelled);
	// A labelled one so the sweep still runs to completion.
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Auxillary, "Deck Jets"));
	hub.ConnectDataHub(data_hub);

	auto& queue = PublishAllAndGetQueue(hub);

	// The labelled device is published; the unlabelled one produced no device topic.
	BOOST_CHECK(FindPayloadContaining(queue, "/device/deck_jets").has_value());

	std::size_t device_topic_count = 0;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/device/") != std::string::npos && !pending.payload.empty())
		{
			++device_topic_count;
		}
	}
	BOOST_CHECK_EQUAL(device_topic_count, 1u);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_ComputeOwnedDeviceTopics_NoDataHub_ReturnsEmpty)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	// No DataHub connected: the weak_ptr does not lock, so nothing is owned.
	auto owned = Test::MqttHubReconcileTest::CallComputeOwnedDeviceTopics(hub);
	BOOST_CHECK(owned.empty());
}

BOOST_AUTO_TEST_CASE(Test_ComputeOwnedDeviceTopics_HaEnabled_IncludesStateTopics)
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	settings.home_assistant_enabled = true;   // owned set also contains the HA short-state topic
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Auxillary, "Pool Light"));
	// A device with no label is skipped by the owned-set builder.
	auto unlabelled = std::make_shared<Kernel::AuxillaryDevice>();
	unlabelled->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(unlabelled);
	hub.ConnectDataHub(data_hub);

	auto owned = Test::MqttHubReconcileTest::CallComputeOwnedDeviceTopics(hub);

	BOOST_CHECK_MESSAGE(owned.contains("test/device/pool_light"), "owned set should contain the device JSON topic");
	BOOST_CHECK_MESSAGE(owned.contains("test/ha/aux_pool_light"), "owned set should contain the HA state topic when HA is enabled");
	// Only the labelled device contributes (JSON + HA state).
	BOOST_CHECK_EQUAL(owned.size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Statistics serialization: connecting a StatisticsHub populates the four
// statistics topics from real hub metrics.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_Statistics)

BOOST_AUTO_TEST_CASE(Test_Statistics_PublishedFromHubMetrics)
{
	enum class TestMsgId { Alpha, Beta };

	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	// Populate two message counters (drives the SerializeStatisticsMessages loop) and a couple of
	// serial metrics so the statistics payloads carry real values.
	++stats_hub->MessageCounts[TestMsgId::Alpha];
	++stats_hub->MessageCounts[TestMsgId::Beta];
	++stats_hub->MessageCounts[TestMsgId::Beta];
	stats_hub->Serial.SerialOverflowCount = 3;
	stats_hub->Serial.TransmissionFailures = 1;

	hub.ConnectDataHub(data_hub);
	hub.ConnectStatisticsHub(stats_hub);

	auto& queue = PublishAllAndGetQueue(hub);

	auto messages = FindPayloadContaining(queue, "/statistics/messages");
	BOOST_REQUIRE(messages.has_value());
	BOOST_REQUIRE(messages->is_array());
	BOOST_CHECK_EQUAL(messages->size(), 2u);   // one array entry per distinct message id

	auto bandwidth = FindPayloadContaining(queue, "/statistics/bandwidth");
	BOOST_REQUIRE(bandwidth.has_value());
	BOOST_CHECK(bandwidth->contains("read"));
	BOOST_CHECK(bandwidth->contains("write"));
	BOOST_CHECK((*bandwidth)["read"].contains("utilisation_1sec"));

	auto latency = FindPayloadContaining(queue, "/statistics/latency");
	BOOST_REQUIRE(latency.has_value());
	BOOST_CHECK(latency->contains("serial_read"));
	BOOST_CHECK(latency->contains("serial_write"));
	BOOST_CHECK(latency->contains("message_processing"));

	auto serial = FindPayloadContaining(queue, "/statistics/serial");
	BOOST_REQUIRE(serial.has_value());
	BOOST_CHECK_EQUAL((*serial)["overflow_count"].get<std::uint64_t>(), 3u);
	BOOST_CHECK_EQUAL((*serial)["transmission_failures"].get<std::uint64_t>(), 1u);

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Poll()'s connected-path branches: the periodic status + statistics publishes,
// the one-shot startup retained-reconcile, and the debounced on-change flush.
// Each needs the hub started AND the client forced connected (so IsConnected()
// gates open) with the relevant deadline already elapsed.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHub_PollPublishing)

namespace
{
	// A hub whose periodic publish intervals are zero, so the very first Poll() sees
	// now >= m_NextStatusPublish / m_NextStatsPublish and runs both periodic branches.
	Options::Mqtt::MqttSettings MakeZeroIntervalSettings()
	{
		auto s = Test::MakeMqttSettings();
		s.status_publish_interval = std::chrono::seconds(0);
		s.statistics_publish_interval = std::chrono::seconds(0);
		return s;
	}
}

BOOST_AUTO_TEST_CASE(Test_Poll_WhenConnected_RunsPeriodicPublishes)
{
	boost::asio::io_context ioc;
	auto settings = MakeZeroIntervalSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();
	hub.ConnectDataHub(data_hub);
	hub.ConnectStatisticsHub(stats_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// Zero intervals => the periodic status + statistics deadlines are already due on the
	// first Poll(), so both periodic branches publish their topics.
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(FindPayloadContaining(queue, "/system/status").has_value());
	BOOST_CHECK(FindPayloadContaining(queue, "/pool/temperatures").has_value());
	BOOST_CHECK(FindPayloadContaining(queue, "/statistics/messages").has_value());

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_Poll_RunsStartupReconcileOnceDeadlineElapsed)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();   // no devices -> nothing owned
	hub.ConnectDataHub(data_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// OnConnected arms the retained-reconcile window and records the device/HA prefixes; a
	// replayed retained topic is then collected while the window is open.
	hub.GetMqttClient()->OnConnected();
	hub.GetMqttClient()->OnMessageReceived("test/device/ghost", R"({"stale":true})");

	// The reconcile deadline is RETAINED_RECONCILE_GRACE (10s) out, so an immediate Poll() does
	// NOT reconcile yet - the collected topic is untouched.
	{
		auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
		auto before = LastEntryForTopicEnding(queue, "/device/ghost");
		hub.Poll();
		auto after = LastEntryForTopicEnding(queue, "/device/ghost");
		BOOST_CHECK_MESSAGE(before.has_value() == after.has_value(),
			"reconcile must not fire before its grace deadline");
	}

	// Force the deadline into the past via the test seam, then Poll() runs the one-shot reconcile,
	// clearing the un-owned collected topic (empty + retained).
	Test::MqttHubReconcileTest::SeedSeenRetainedTopics(hub, { std::string("test/device/ghost") });
	Test::MqttHubReconcileTest::ExpireRetainedReconcileDeadline(hub);
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto cleared = LastEntryForTopicEnding(queue, "/device/ghost");
	BOOST_REQUIRE_MESSAGE(cleared.has_value(), "Poll() should have reconciled the collected topic");
	BOOST_CHECK(cleared->first.empty());
	BOOST_CHECK(cleared->second);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_DataHubConfigChange_ArmsOnChangePublish_FlushedByPoll)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubTestSettings();
	settings.publish_on_change = true;
	// Large periodic intervals so the flush we observe is the on-change one, not a periodic tick.
	settings.status_publish_interval = std::chrono::seconds(3600);
	settings.statistics_publish_interval = std::chrono::seconds(3600);
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	hub.ConnectDataHub(data_hub);

	hub.Start();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// A DataHub value change fires ConfigUpdateSignal with a real config event. With the hub
	// running + connected + publish_on_change, OnDataHubConfigChanged arms the debounced publish.
	data_hub->AirTemp(Celsius(24.0));

	// The on-change deadline is ON_CHANGE_DEBOUNCE (250ms) out; wait past it, then Poll() flushes
	// the coalesced on-change publish (system + pool + device status).
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK_MESSAGE(FindPayloadContaining(queue, "/system/status").has_value(),
		"the on-change flush should have published system status");
	BOOST_CHECK(FindPayloadContaining(queue, "/pool/temperatures").has_value());

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
