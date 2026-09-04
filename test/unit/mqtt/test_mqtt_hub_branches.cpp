#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_events/equipment_hub_system_event.h"
#include "kernel/hub_events/hub_eventtypes.h"
#include "kernel/temperature.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

//=============================================================================
// MqttHub branch tests.
//
// These complement test_mqtt_hub.cpp with the arms it leaves unexercised: the
// on-change debounce deadline NOT yet reached, the EquipmentHub status-change
// signal (and its not-running / on-change-disabled guards), a body that is
// neither Pool nor Spa in the per-body temperature publish, a device that
// persists across two sweeps (no retained clear), a chlorinator with a resolved
// setpoint, the owned-topic set with HA disabled across every category, a
// reconcile that clears nothing, the reconcile-window collection filters, and
// the POOLHT2 flag in the temperatures payload.
//=============================================================================

namespace
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	Options::Mqtt::MqttSettings MakeHubBranchSettings()
	{
		return Test::MakeMqttSettings();
	}

	// Periodic intervals far in the future, so the only publishes observed are the
	// ones the test itself triggers (on-change flush, PublishAllStatus, ...).
	Options::Mqtt::MqttSettings MakeQuietPeriodicSettings()
	{
		auto s = MakeHubBranchSettings();
		s.status_publish_interval = std::chrono::seconds(3600);
		s.statistics_publish_interval = std::chrono::seconds(3600);
		return s;
	}

	Kernel::Temperature Celsius(double c)
	{
		return Kernel::Temperature::ConvertToTemperatureInCelsius(c);
	}

	std::shared_ptr<Kernel::AuxillaryDevice> MakeDeviceOfType(Traits::AuxillaryTypes type, const std::string& label)
	{
		auto dev = std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		return dev;
	}

	// A concrete EquipmentHub system event (the base is abstract on ToJSON).
	class TestSystemEvent : public Kernel::EquipmentHub_SystemEvent
	{
	public:
		TestSystemEvent()
			: Kernel::EquipmentHub_SystemEvent(Kernel::Hub_EventTypes::ServiceStatus)
		{
		}

		nlohmann::json ToJSON() const override { return nlohmann::json::object(); }
	};

	// First published payload whose topic contains `needle` (parsed as JSON), or nullopt.
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

	template <typename QueueT>
	std::size_t CountTopicEnding(const QueueT& queue, const std::string& suffix)
	{
		std::size_t n = 0;
		for (const auto& pending : queue)
		{
			if (pending.topic.ends_with(suffix)) { ++n; }
		}
		return n;
	}

	template <typename QueueT>
	bool AnyEmptyRetainedEnding(const QueueT& queue, const std::string& suffix)
	{
		for (const auto& pending : queue)
		{
			if (pending.topic.ends_with(suffix) && pending.payload.empty() && pending.retain) { return true; }
		}
		return false;
	}

	// Start the hub and force its client connected so the publish paths enqueue.
	void StartConnected(Mqtt::MqttHub& hub)
	{
		hub.Start();
		Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
	}
}

//=============================================================================
// Poll(): on-change debounce + EquipmentHub-driven on-change publishes
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHubBranches_OnChange)

BOOST_AUTO_TEST_CASE(Test_Poll_OnChangePending_BeforeDebounceDeadline_DoesNotPublish)
{
	boost::asio::io_context ioc;
	auto settings = MakeQuietPeriodicSettings();
	settings.publish_on_change = true;
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	hub.ConnectDataHub(data_hub);

	auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	hub.SetSteadyClock([&] { return fake_now; });

	StartConnected(hub);
	data_hub->AirTemp(Celsius(24.0));   // arms the debounced on-change publish

	// Still inside the debounce window: pending but not yet due -> nothing published.
	hub.Poll();
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(!FindPayloadContaining(queue, "/system/status").has_value());
	BOOST_CHECK(!FindPayloadContaining(queue, "/pool/temperatures").has_value());

	// Past the window: the same pending change is flushed exactly once.
	fake_now += std::chrono::milliseconds(300);
	hub.Poll();
	BOOST_CHECK_EQUAL(CountTopicEnding(queue, "/system/status"), 1u);
	BOOST_CHECK_EQUAL(CountTopicEnding(queue, "/pool/temperatures"), 1u);

	// And once flushed, a further Poll() with nothing new publishes nothing more.
	// MqttHub::Poll() drains the client's pending-publish queue at its start, so the
	// entries counted above are gone by the time the on-change block runs again; the
	// queue is therefore empty unless that block enqueued something new (it must not).
	fake_now += std::chrono::milliseconds(300);
	hub.Poll();
	BOOST_CHECK_EQUAL(CountTopicEnding(queue, "/system/status"), 0u);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_EquipmentStatusChange_ArmsOnChangePublish_FlushedByPoll)
{
	boost::asio::io_context ioc;
	auto settings = MakeQuietPeriodicSettings();
	settings.publish_on_change = true;
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	hub.ConnectDataHub(data_hub);
	hub.ConnectEquipmentHub(equip_hub);

	auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	hub.SetSteadyClock([&] { return fake_now; });

	StartConnected(hub);

	// An equipment status change (running + connected + on-change enabled) arms the publish.
	equip_hub->EquipmentStatusChangeSignal(std::make_shared<TestSystemEvent>());

	fake_now += std::chrono::milliseconds(300);
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK_MESSAGE(FindPayloadContaining(queue, "/system/status").has_value(),
		"an equipment status change should schedule an on-change publish");
	BOOST_CHECK(FindPayloadContaining(queue, "/pool/circulation").has_value());

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_EquipmentStatusChange_BeforeStart_IsIgnored)
{
	boost::asio::io_context ioc;
	auto settings = MakeQuietPeriodicSettings();
	settings.publish_on_change = true;
	Mqtt::MqttHub hub(ioc, settings);

	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	hub.ConnectEquipmentHub(equip_hub);

	auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	hub.SetSteadyClock([&] { return fake_now; });

	// Not running yet: the handler's IsRunning() guard drops the change.
	equip_hub->EquipmentStatusChangeSignal(std::make_shared<TestSystemEvent>());

	StartConnected(hub);
	fake_now += std::chrono::seconds(1);
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(!FindPayloadContaining(queue, "/system/status").has_value());

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_EquipmentStatusChange_OnChangeDisabled_IsIgnored)
{
	boost::asio::io_context ioc;
	auto settings = MakeQuietPeriodicSettings();
	settings.publish_on_change = false;
	Mqtt::MqttHub hub(ioc, settings);

	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	hub.ConnectEquipmentHub(equip_hub);

	auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	hub.SetSteadyClock([&] { return fake_now; });

	StartConnected(hub);

	// Running + connected, but publish_on_change is off: nothing is armed.
	equip_hub->EquipmentStatusChangeSignal(std::make_shared<TestSystemEvent>());
	// A null event is dropped by the same guard.
	equip_hub->EquipmentStatusChangeSignal(nullptr);

	fake_now += std::chrono::seconds(1);
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(!FindPayloadContaining(queue, "/system/status").has_value());

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Pool / device status serialisation arms
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHubBranches_Serialization)

// A body that is neither Pool nor Spa (e.g. a Shared body) takes the default
// freshness arm: no channel to read, so its current temperature is unavailable.
BOOST_AUTO_TEST_CASE(Test_PublishPoolStatus_SharedBody_PublishedWithUnavailableCurrent)
{
	boost::asio::io_context ioc;
	Mqtt::MqttHub hub(ioc, MakeHubBranchSettings());

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->AddBody(Kernel::BodyOfWater{ Kernel::BodyOfWaterIds::Shared, "Shared" });
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);
	hub.PublishAllStatus();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto body = FindPayloadContaining(queue, "/body/shared/temperature");
	BOOST_REQUIRE_MESSAGE(body.has_value(), "the shared body should be published to body/shared/temperature");
	BOOST_REQUIRE(body->contains("current"));
	BOOST_CHECK((*body)["current"].is_null());
	BOOST_CHECK_EQUAL((*body)["is_active"].get<bool>(), false);

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStatus_UnchangedDevice_RepublishedNotCleared)
{
	boost::asio::io_context ioc;
	Mqtt::MqttHub hub(ioc, MakeHubBranchSettings());

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Auxillary, "Aux5"));
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);
	hub.PublishAllStatus();
	hub.PublishAllStatus();

	// Present in both sweeps: published twice, never cleared with an empty retained payload.
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK_EQUAL(CountTopicEnding(queue, "/device/aux5"), 2u);
	BOOST_CHECK(!AnyEmptyRetainedEnding(queue, "/device/aux5"));

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_PublishDeviceStatus_ChlorinatorWithSetpoint_PublishesSetpointPercent)
{
	boost::asio::io_context ioc;
	Mqtt::MqttHub hub(ioc, MakeHubBranchSettings());

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto chlorinator = MakeDeviceOfType(Traits::AuxillaryTypes::Chlorinator, "AquaPure");
	chlorinator->AuxillaryTraits.Set(Traits::ChlorinatorPoolSetpointTrait{}, static_cast<std::uint8_t>(55));
	data_hub->Devices.Add(chlorinator);
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);
	hub.PublishAllStatus();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto payload = FindPayloadContaining(queue, "/device/aquapure");
	BOOST_REQUIRE(payload.has_value());
	BOOST_CHECK_EQUAL((*payload)["type"].get<std::string>(), "chlorinator");
	BOOST_REQUIRE(payload->contains("setpoint_percent"));
	BOOST_CHECK_EQUAL((*payload)["setpoint_percent"].get<int>(), 55);
	BOOST_REQUIRE(payload->contains("generating_reason"));
	BOOST_CHECK(!(*payload)["generating_reason"].get<std::string>().empty());

	hub.Stop();
}

BOOST_AUTO_TEST_CASE(Test_SerializeTemperatures_PoolHeater2Enabled_PublishedAsBool)
{
	boost::asio::io_context ioc;
	Mqtt::MqttHub hub(ioc, MakeHubBranchSettings());

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->PoolHeater2Enabled(true);
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);
	hub.PublishAllStatus();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	auto temps = FindPayloadContaining(queue, "/pool/temperatures");
	BOOST_REQUIRE(temps.has_value());
	BOOST_REQUIRE(temps->contains("pool_heater_2_enabled"));
	BOOST_CHECK((*temps)["pool_heater_2_enabled"].is_boolean());
	BOOST_CHECK_EQUAL((*temps)["pool_heater_2_enabled"].get<bool>(), true);

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Retained-topic ownership + reconciliation arms
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttHubBranches_Reconcile)

BOOST_AUTO_TEST_CASE(Test_ComputeOwnedDeviceTopics_HaDisabled_JsonTopicsForEveryCategory)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubBranchSettings();
	settings.home_assistant_enabled = false;
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Auxillary, "Pool Light"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Heater, "Pool Heater"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Pump, "Filter Pump"));
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Chlorinator, "AquaPure"));
	hub.ConnectDataHub(data_hub);

	auto owned = Test::MqttHubReconcileTest::CallComputeOwnedDeviceTopics(hub);

	BOOST_CHECK_EQUAL(owned.size(), 4u);   // JSON topic per device, no HA state topics
	BOOST_CHECK(owned.contains("test/device/pool_light"));
	BOOST_CHECK(owned.contains("test/device/pool_heater"));
	BOOST_CHECK(owned.contains("test/device/filter_pump"));
	BOOST_CHECK(owned.contains("test/device/aquapure"));
	for (const auto& topic : owned)
	{
		BOOST_CHECK_MESSAGE(topic.find("/ha/") == std::string::npos, "HA disabled must not own an HA state topic: " + topic);
	}
}

BOOST_AUTO_TEST_CASE(Test_Reconcile_AllSeenTopicsOwned_ClearsNothing)
{
	boost::asio::io_context ioc;
	auto settings = MakeHubBranchSettings();
	settings.home_assistant_enabled = true;
	Mqtt::MqttHub hub(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	data_hub->Devices.Add(MakeDeviceOfType(Traits::AuxillaryTypes::Pump, "Filter Pump"));
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);

	// Everything the broker replayed is still owned by the current device set.
	Test::MqttHubReconcileTest::SeedSeenRetainedTopics(hub, { "test/device/filter_pump", "test/ha/pump_filter_pump" });
	Test::MqttHubReconcileTest::CallReconcileRetainedTopics(hub);

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(!AnyEmptyRetainedEnding(queue, "/device/filter_pump"));
	BOOST_CHECK(!AnyEmptyRetainedEnding(queue, "/ha/pump_filter_pump"));

	hub.Stop();
}

// While the reconcile window is open only NON-EMPTY payloads on the device/ or
// ha/ namespaces are collected; an empty (already-cleared) retained message and
// a foreign topic are ignored, so the later reconcile never touches them.
BOOST_AUTO_TEST_CASE(Test_HandleMessage_DuringReconcileWindow_FiltersEmptyAndForeignTopics)
{
	boost::asio::io_context ioc;
	Mqtt::MqttHub hub(ioc, MakeQuietPeriodicSettings());

	auto data_hub = std::make_shared<Kernel::DataHub>();   // no devices -> nothing owned
	hub.ConnectDataHub(data_hub);

	StartConnected(hub);
	hub.GetMqttClient()->OnConnected();   // arms the window + records the topic prefixes

	hub.GetMqttClient()->OnMessageReceived("test/device/ghost_empty", "");            // empty -> not collected
	hub.GetMqttClient()->OnMessageReceived("test/other/ghost_foreign", R"({"x":1})"); // foreign namespace -> not collected
	hub.GetMqttClient()->OnMessageReceived("test/ha/aux_ghost", "On");               // HA namespace -> collected

	Test::MqttHubReconcileTest::ExpireRetainedReconcileDeadline(hub);
	hub.Poll();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	BOOST_CHECK(!AnyEmptyRetainedEnding(queue, "/device/ghost_empty"));
	BOOST_CHECK(!AnyEmptyRetainedEnding(queue, "/other/ghost_foreign"));
	BOOST_CHECK_MESSAGE(AnyEmptyRetainedEnding(queue, "/ha/aux_ghost"),
		"an un-owned retained HA state topic collected in the window must be cleared");

	hub.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
