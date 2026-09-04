#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <nlohmann/json.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_integration.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

//=============================================================================
// MqttIntegration branch tests.
//
// These complement test_mqtt_integration.cpp with the command-handler arms a
// well-behaved dispatcher never reaches: every CommandResult -> response status
// mapping, a UUID device_id, a dispatcher that throws (each handler's catch),
// the dispatcher-less dynamic device/heater/chlorinator/circulation handlers,
// setpoint dispatch without a DataHub / with a non-finite wire value / with a
// missing target, the Celsius display-preference arm, the display-units-changed
// republish, duplicate-label and already-registered dynamic commands, and the
// HA seed's "other topic" / "seed already consumed" arms.
//=============================================================================

namespace
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	using CommandResult = Interfaces::ICommandDispatcher::CommandResult;
	using DeviceAction = Interfaces::ICommandDispatcher::DeviceAction;

	Options::Mqtt::MqttSettings MakeBranchSettings()
	{
		auto s = Test::MakeMqttSettings();
		s.home_assistant_enabled = true;
		s.ha_discovery_prefix = "homeassistant";
		s.ha_device_id = "aqualink_branch";
		return s;
	}

	constexpr const char* HA_CONFIG_TOPIC = "homeassistant/device/aqualink_branch/config";

	// A dispatcher whose results and failure modes are scripted per test, and which
	// records every call so the routing can be asserted.
	class ScriptedDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult toggle_result{ CommandResult::Success };
		bool throw_on_toggle_label{ false };          // std::logic_error -> the handler's outer catch
		bool throw_runtime_on_toggle_uuid{ false };   // std::runtime_error -> the UUID-parse fallback catch
		bool throw_on_setpoint{ false };
		bool throw_on_command_by_label{ false };
		bool throw_on_heater{ false };

		std::vector<boost::uuids::uuid> uuid_calls;
		std::vector<std::string> label_calls;
		std::vector<std::pair<std::string, DeviceAction>> device_calls;
		std::vector<std::uint8_t> pool_setpoints;
		std::vector<std::uint8_t> spa_setpoints;
		std::vector<std::pair<Kernel::BodyOfWaterIds, bool>> heater_calls;

		CommandResult ToggleByUuid(const boost::uuids::uuid& uuid) override
		{
			if (throw_runtime_on_toggle_uuid) { throw std::runtime_error("scripted uuid failure"); }
			uuid_calls.push_back(uuid);
			return toggle_result;
		}
		CommandResult ToggleByLabel(const std::string& label) override
		{
			if (throw_on_toggle_label) { throw std::logic_error("scripted label failure"); }
			label_calls.push_back(label);
			return toggle_result;
		}
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string& label, DeviceAction action) override
		{
			if (throw_on_command_by_label) { throw std::runtime_error("scripted device failure"); }
			device_calls.emplace_back(label, action);
			return CommandResult::Success;
		}
		CommandResult SetPoolSetpoint(std::uint8_t value) override
		{
			if (throw_on_setpoint) { throw std::runtime_error("scripted setpoint failure"); }
			pool_setpoints.push_back(value);
			return CommandResult::Success;
		}
		CommandResult SetSpaSetpoint(std::uint8_t value) override
		{
			if (throw_on_setpoint) { throw std::runtime_error("scripted setpoint failure"); }
			spa_setpoints.push_back(value);
			return CommandResult::Success;
		}
		CommandResult SetChlorinatorPercentage(std::uint8_t, Kernel::BodyOfWaterIds) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds body, bool enable) override
		{
			if (throw_on_heater) { throw std::runtime_error("scripted heater failure"); }
			heater_calls.emplace_back(body, enable);
			return CommandResult::Success;
		}
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	// Last queued payload (parsed as JSON) whose topic contains `needle`, or nullopt.
	// Templated so the client's private PendingPublish type is never named.
	template <typename QueueT>
	std::optional<nlohmann::json> LastPayloadOnTopic(const QueueT& queue, const std::string& needle)
	{
		std::optional<nlohmann::json> found;
		for (const auto& pending : queue)
		{
			if (pending.topic.find(needle) != std::string::npos)
			{
				found = nlohmann::json::parse(pending.payload);
			}
		}
		return found;
	}

	template <typename QueueT>
	std::size_t CountOnExactTopic(const QueueT& queue, const std::string& topic)
	{
		std::size_t n = 0;
		for (const auto& pending : queue)
		{
			if (pending.topic == topic) { ++n; }
		}
		return n;
	}

	struct BranchFixture
	{
		enum class Wiring
		{
			Full,             // hubs + dispatcher
			FullWithPrefs,    // hubs + dispatcher + preferences hub
			NoDispatcher,     // hubs only
			DispatcherOnly    // dispatcher only (no DataHub)
		};

		boost::asio::io_context ioc;
		std::shared_ptr<ScriptedDispatcher> dispatcher{ std::make_shared<ScriptedDispatcher>() };
		std::shared_ptr<Kernel::DataHub> data_hub{ std::make_shared<Kernel::DataHub>() };
		std::shared_ptr<Kernel::EquipmentHub> equip_hub{ std::make_shared<Kernel::EquipmentHub>() };
		std::shared_ptr<Kernel::StatisticsHub> stats_hub{ std::make_shared<Kernel::StatisticsHub>() };
		std::shared_ptr<Kernel::PreferencesHub> prefs{ std::make_shared<Kernel::PreferencesHub>() };
		Mqtt::MqttIntegration integration;

		BranchFixture()
			: integration(ioc, MakeBranchSettings())
		{
		}

		~BranchFixture()
		{
			integration.Stop();
		}

		void AddAux(const std::string& label)
		{
			auto aux = std::make_shared<Kernel::AuxillaryDevice>();
			aux->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
			aux->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
			data_hub->Devices.Add(aux);
		}

		void AddHeater(const std::string& label, Kernel::BodyOfWaterIds body, bool with_label = true, bool with_body = true)
		{
			auto heater = std::make_shared<Kernel::AuxillaryDevice>();
			heater->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Heater);
			if (with_label) { heater->AuxillaryTraits.Set(Traits::LabelTrait{}, label); }
			if (with_body) { heater->AuxillaryTraits.Set(Traits::BodyOfWaterTrait{}, body); }
			data_hub->Devices.Add(heater);
		}

		// Wire the requested hubs through the locator, start, force the client
		// connected (so PublishCustom responses enqueue) and run the dynamic
		// command registration pass.
		Mqtt::MqttHub& ConnectAndPublish(Wiring wiring = Wiring::Full)
		{
			Kernel::HubLocator locator;
			if (wiring != Wiring::DispatcherOnly)
			{
				locator.Register(data_hub).Register(equip_hub).Register(stats_hub);
			}
			if (wiring == Wiring::FullWithPrefs)
			{
				locator.Register(prefs);
			}
			if (wiring != Wiring::NoDispatcher)
			{
				locator.Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
			}

			integration.ConnectHubs(locator);
			integration.Start();

			auto hub = integration.GetMqttHub();
			BOOST_REQUIRE(hub != nullptr);
			Test::MqttClientPacketTest::ForceConnectedState(*hub->GetMqttClient());
			hub->OnDevicesPublished();
			return *hub;
		}

		static void Send(Mqtt::MqttHub& hub, const std::string& command, const std::string& payload)
		{
			hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic(command), payload);
		}

		static auto& Queue(Mqtt::MqttHub& hub)
		{
			return Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
		}
	};
}

//=============================================================================
// Generic "device" command: UUID routing, result-string mapping, throwing dispatcher
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegrationBranches_DeviceCommand, BranchFixture)

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_ValidUuid_RoutesToToggleByUuid)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device"));

	Send(hub, "device", R"({"device_id":"01234567-89ab-cdef-0123-456789abcdef"})");

	BOOST_REQUIRE_EQUAL(dispatcher->uuid_calls.size(), 1u);
	BOOST_CHECK(dispatcher->label_calls.empty());

	auto response = LastPayloadOnTopic(Queue(hub), "/response/device");
	BOOST_REQUIRE(response.has_value());
	BOOST_CHECK_EQUAL(response->value("command", ""), "device");
	BOOST_CHECK_EQUAL(response->value("status", ""), "success");
	BOOST_CHECK_EQUAL(response->value("device_id", ""), "01234567-89ab-cdef-0123-456789abcdef");
	BOOST_CHECK_EQUAL(response->value("action", ""), "toggle");   // defaulted when absent
	BOOST_CHECK(response->contains("timestamp"));
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_EveryResult_MapsToResponseStatus)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device"));

	const std::vector<std::pair<CommandResult, std::string>> expectations = {
		{ CommandResult::DeviceNotFound,       "device_not_found" },
		{ CommandResult::NoSerialAdapter,      "no_serial_adapter" },
		{ CommandResult::UnknownEquipmentType, "unknown_equipment_type" },
		{ CommandResult::InvalidValue,         "invalid_value" },
		{ CommandResult::Busy,                 "error" },   // no dedicated string -> generic error
		{ CommandResult::Success,              "success" },
	};

	for (const auto& [result, expected_status] : expectations)
	{
		dispatcher->toggle_result = result;
		Send(hub, "device", R"({"device_id":"Pool Light","action":"toggle"})");

		auto response = LastPayloadOnTopic(Queue(hub), "/response/device");
		BOOST_REQUIRE(response.has_value());
		BOOST_CHECK_EQUAL(response->value("status", ""), expected_status);
	}

	BOOST_CHECK_EQUAL(dispatcher->label_calls.size(), expectations.size());
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_DispatcherThrows_IsContainedAndNoResponse)
{
	dispatcher->throw_on_toggle_label = true;
	auto& hub = ConnectAndPublish();

	BOOST_CHECK_NO_THROW(Send(hub, "device", R"({"device_id":"Pool Light"})"));

	// The outer catch swallows the failure before the response is built.
	BOOST_CHECK(!LastPayloadOnTopic(Queue(hub), "/response/device").has_value());
	BOOST_CHECK(dispatcher->label_calls.empty());
}

// A runtime_error escaping ToggleByUuid is indistinguishable from a UUID parse
// failure to the handler, so it falls back to the label route with the raw id.
BOOST_AUTO_TEST_CASE(Test_DeviceCommand_UuidDispatchThrowsRuntimeError_FallsBackToLabel)
{
	dispatcher->throw_runtime_on_toggle_uuid = true;
	auto& hub = ConnectAndPublish();

	const std::string id = "01234567-89ab-cdef-0123-456789abcdef";
	Send(hub, "device", nlohmann::json{ {"device_id", id} }.dump());

	BOOST_CHECK(dispatcher->uuid_calls.empty());
	BOOST_REQUIRE_EQUAL(dispatcher->label_calls.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->label_calls[0], id);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Setpoint commands: missing target, no DataHub, non-finite wire value,
// Celsius preference, throwing dispatcher (JSON + HA number handlers)
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegrationBranches_Setpoint, BranchFixture)

BOOST_AUTO_TEST_CASE(Test_SetpointCommand_MissingTarget_NotDispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint"));

	Send(hub, "setpoint", R"({"temperature":28.0})");

	BOOST_CHECK(dispatcher->pool_setpoints.empty());
	BOOST_CHECK(dispatcher->spa_setpoints.empty());
	BOOST_CHECK(!LastPayloadOnTopic(Queue(hub), "/response/setpoint").has_value());
}

// Without a DataHub the system units are unknown, so the Celsius value is
// converted to Fahrenheit for the wire (the conservative default).
BOOST_AUTO_TEST_CASE(Test_SetpointCommand_NoDataHub_ConvertsToFahrenheitWire)
{
	auto& hub = ConnectAndPublish(Wiring::DispatcherOnly);
	BOOST_REQUIRE(hub.HasCommand("setpoint"));

	Send(hub, "setpoint", R"({"target":"spa","temperature":30.0})");

	BOOST_REQUIRE_EQUAL(dispatcher->spa_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->spa_setpoints[0], 86u);   // 30C -> 86F

	auto response = LastPayloadOnTopic(Queue(hub), "/response/setpoint");
	BOOST_REQUIRE(response.has_value());
	BOOST_CHECK_EQUAL(response->value("status", ""), "success");
	BOOST_CHECK_EQUAL(response->value("target", ""), "spa");
}

// A finite JSON value whose Fahrenheit conversion overflows to infinity must
// hit the non-finite guard and dispatch 0 rather than an undefined cast.
BOOST_AUTO_TEST_CASE(Test_SetpointCommand_OverflowingConversion_DispatchesZero)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Fahrenheit);
	auto& hub = ConnectAndPublish();

	Send(hub, "setpoint", R"({"target":"pool","temperature":1e308})");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 0u);
}

BOOST_AUTO_TEST_CASE(Test_SetpointCommand_DispatcherThrows_IsContained)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	dispatcher->throw_on_setpoint = true;
	auto& hub = ConnectAndPublish();

	BOOST_CHECK_NO_THROW(Send(hub, "setpoint", R"({"target":"pool","temperature":28.0})"));

	BOOST_CHECK(dispatcher->pool_setpoints.empty());
	BOOST_CHECK(!LastPayloadOnTopic(Queue(hub), "/response/setpoint").has_value());
}

BOOST_AUTO_TEST_CASE(Test_HaSetpointCommand_DispatcherThrows_IsContained)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	dispatcher->throw_on_setpoint = true;
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint/spa"));

	BOOST_CHECK_NO_THROW(Send(hub, "setpoint/spa", "38"));

	BOOST_CHECK(dispatcher->spa_setpoints.empty());
	BOOST_CHECK(!LastPayloadOnTopic(Queue(hub), "/response/setpoint").has_value());
}

// A connected preferences hub set to Celsius: the HA number value is taken as
// Celsius (no F->C normalisation) and lands on the wire unchanged.
BOOST_AUTO_TEST_CASE(Test_HaSetpoint_CelsiusPreference_DispatchesUnconverted)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	prefs->Temperature_DisplayUnits = Kernel::TemperatureUnits::Celsius;
	auto& hub = ConnectAndPublish(Wiring::FullWithPrefs);
	BOOST_REQUIRE(hub.HasCommand("setpoint/pool"));

	Send(hub, "setpoint/pool", "31");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 31u);

	auto response = LastPayloadOnTopic(Queue(hub), "/response/setpoint");
	BOOST_REQUIRE(response.has_value());
	BOOST_CHECK_EQUAL(response->value("status", ""), "success");
	BOOST_CHECK_EQUAL(response->value("temperature", 0.0), 31.0);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Dynamic device / heater / chlorinator / circulation handlers without a
// dispatcher, with a throwing dispatcher, and the registration guards
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegrationBranches_DynamicCommands, BranchFixture)

BOOST_AUTO_TEST_CASE(Test_DynamicCommands_NoDispatcher_AreRegisteredAndRejectSafely)
{
	AddAux("Pool Light");
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool);
	auto& hub = ConnectAndPublish(Wiring::NoDispatcher);

	// Registration does not depend on a dispatcher being resolvable...
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));
	BOOST_REQUIRE(hub.HasCommand("heater/pool_heater"));
	BOOST_REQUIRE(hub.HasCommand("chlorinator/percentage"));
	BOOST_REQUIRE(hub.HasCommand("chlorinator/spa/percentage"));
	BOOST_REQUIRE(hub.HasCommand("chlorinator/boost"));
	BOOST_REQUIRE(hub.HasCommand("circulation/mode"));

	// ...but every handler locks a null dispatcher and returns without throwing.
	BOOST_CHECK_NO_THROW(Send(hub, "device/pool_light", "ON"));
	BOOST_CHECK_NO_THROW(Send(hub, "heater/pool_heater", "OFF"));
	BOOST_CHECK_NO_THROW(Send(hub, "chlorinator/percentage", "50"));
	BOOST_CHECK_NO_THROW(Send(hub, "chlorinator/spa/percentage", "70"));
	BOOST_CHECK_NO_THROW(Send(hub, "chlorinator/boost", "ON"));
	BOOST_CHECK_NO_THROW(Send(hub, "circulation/mode", "spa"));

	BOOST_CHECK(dispatcher->device_calls.empty());
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_DynamicDeviceCommand_DispatcherThrows_IsContained)
{
	AddAux("Pool Light");
	dispatcher->throw_on_command_by_label = true;
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));

	BOOST_CHECK_NO_THROW(Send(hub, "device/pool_light", "OFF"));
	BOOST_CHECK(dispatcher->device_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_DispatcherThrows_IsContained)
{
	AddHeater("Spa Heater", Kernel::BodyOfWaterIds::Spa);
	dispatcher->throw_on_heater = true;
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("heater/spa_heater"));

	BOOST_CHECK_NO_THROW(Send(hub, "heater/spa_heater", "ON"));
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_NotRegistered_WhenLabelMissing)
{
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool, /*with_label=*/false, /*with_body=*/true);
	auto& hub = ConnectAndPublish();

	BOOST_CHECK(!hub.HasCommand("heater/pool_heater"));
	BOOST_CHECK(!hub.HasCommand("heater/"));
	// The DataHub still lists the heater; it is the missing label that skips registration.
	BOOST_CHECK_EQUAL(data_hub->Heaters().size(), 1u);
}

// A second registration pass (every device publish re-runs it) must find each
// dynamic command already present and neither duplicate nor replace it.
BOOST_AUTO_TEST_CASE(Test_DynamicCommands_SecondPass_SkipsAlreadyRegistered)
{
	AddAux("Pool Light");
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool);
	auto& hub = ConnectAndPublish();

	const auto count_after_first = hub.CommandCount();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));
	BOOST_REQUIRE(hub.HasCommand("heater/pool_heater"));

	hub.OnDevicesPublished();   // second pass: every HasCommand() guard trips

	BOOST_CHECK_EQUAL(hub.CommandCount(), count_after_first);

	Send(hub, "device/pool_light", "ON");
	Send(hub, "heater/pool_heater", "ON");
	BOOST_REQUIRE_EQUAL(dispatcher->device_calls.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->device_calls[0].first, "Pool Light");
	BOOST_CHECK(dispatcher->device_calls[0].second == DeviceAction::On);
	BOOST_REQUIRE_EQUAL(dispatcher->heater_calls.size(), 1u);
	BOOST_CHECK(dispatcher->heater_calls[0].first == Kernel::BodyOfWaterIds::Pool);
	BOOST_CHECK(dispatcher->heater_calls[0].second);
}

// Two devices carrying the SAME label are not a slug collision (no warning, no
// shadowing): the topic is registered once and drives that label.
BOOST_AUTO_TEST_CASE(Test_DynamicDeviceCommand_DuplicateLabels_RegisterOnce)
{
	AddAux("Pool Light");
	AddAux("Pool Light");
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));

	Send(hub, "device/pool_light", "OFF");

	BOOST_REQUIRE_EQUAL(dispatcher->device_calls.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->device_calls[0].first, "Pool Light");
	BOOST_CHECK(dispatcher->device_calls[0].second == DeviceAction::Off);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Home Assistant wiring: display-units republish + seed-window edge arms
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegrationBranches_HaWiring, BranchFixture)

BOOST_AUTO_TEST_CASE(Test_DisplayUnitsChanged_RepublishesDiscoveryConfig)
{
	auto& hub = ConnectAndPublish(Wiring::FullWithPrefs);
	const auto before = CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC);
	BOOST_REQUIRE_GE(before, 1u);   // the device-publish pass already published discovery once

	prefs->DisplayUnitsChangedSignal();

	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before + 1);
}

BOOST_AUTO_TEST_CASE(Test_DisplayUnitsChanged_WithoutPreferencesHub_NotSubscribed)
{
	auto& hub = ConnectAndPublish(Wiring::Full);   // prefs never registered
	const auto before = CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC);

	prefs->DisplayUnitsChangedSignal();   // nobody is listening

	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before);
}

BOOST_AUTO_TEST_CASE(Test_HaSeed_OtherTopicWhilePending_DoesNotConsumeSeed)
{
	auto& hub = ConnectAndPublish();
	hub.GetMqttClient()->OnConnected();   // arms the seed window
	const auto before = CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC);

	// A retained config for some OTHER device is not ours: the seed stays pending.
	hub.GetMqttClient()->OnMessageReceived("homeassistant/device/someone_else/config", R"({"cmps":{}})");
	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before);

	// Ours consumes the seed and republishes...
	hub.GetMqttClient()->OnMessageReceived(HA_CONFIG_TOPIC, R"({"cmps":{}})");
	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before + 1);

	// ...and a second replay after the seed is consumed is ignored.
	hub.GetMqttClient()->OnMessageReceived(HA_CONFIG_TOPIC, R"({"cmps":{}})");
	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before + 1);
}

// With the seed pending, a device publish must NOT republish discovery (it would
// clobber the retained config before it is read back) but still refreshes states.
BOOST_AUTO_TEST_CASE(Test_HaSeed_DevicesPublishedWhilePending_HoldsDiscovery)
{
	AddAux("Pool Light");
	auto& hub = ConnectAndPublish();
	hub.GetMqttClient()->OnConnected();
	const auto before = CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC);
	const auto states_before = CountOnExactTopic(Queue(hub), "test/ha/aux_pool_light");

	hub.OnDevicesPublished();

	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), HA_CONFIG_TOPIC), before);
	BOOST_CHECK_EQUAL(CountOnExactTopic(Queue(hub), "test/ha/aux_pool_light"), states_before + 1);
}

BOOST_AUTO_TEST_SUITE_END()
