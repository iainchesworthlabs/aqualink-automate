#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "mqtt/mqtt_integration.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_client.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

namespace
{
	Options::Mqtt::MqttSettings MakeEnabledSettings()
	{
		return Test::MakeMqttSettings();
	}

	Options::Mqtt::MqttSettings MakeDisabledSettings()
	{
		Options::Mqtt::MqttSettings s;
		s.enabled = false;
		return s;
	}

	Options::Mqtt::MqttSettings MakeHaEnabledSettings()
	{
		auto s = Test::MakeMqttSettings();
		s.home_assistant_enabled = true;
		s.ha_discovery_prefix = "homeassistant";
		s.ha_device_id = "aqualink_test";
		return s;
	}
}

//=============================================================================
// MqttIntegration construction tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_Construction)

BOOST_AUTO_TEST_CASE(Test_Construction_WhenEnabled_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();

	BOOST_CHECK_NO_THROW(Mqtt::MqttIntegration integration(ioc, settings));
}

BOOST_AUTO_TEST_CASE(Test_Construction_WhenDisabled_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();

	BOOST_CHECK_NO_THROW(Mqtt::MqttIntegration integration(ioc, settings));
}

BOOST_AUTO_TEST_CASE(Test_Construction_WhenEnabled_CreatesHub)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(integration.GetMqttHub() != nullptr);
}

BOOST_AUTO_TEST_CASE(Test_Construction_WhenDisabled_NoHub)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(integration.GetMqttHub() == nullptr);
}

BOOST_AUTO_TEST_CASE(Test_Construction_WithHomeAssistant_ConfiguresLWT)
{
	boost::asio::io_context ioc;
	auto settings = MakeHaEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);

	auto client = hub->GetMqttClient();
	BOOST_REQUIRE(client != nullptr);

	// When HA is enabled, LWT should be configured on the client
	auto& will = client->GetWill();
	BOOST_REQUIRE(will.has_value());
	BOOST_CHECK_EQUAL(will->topic, "test/status/availability");
	BOOST_CHECK_EQUAL(will->payload, "offline");
	BOOST_CHECK(will->retain);
}

BOOST_AUTO_TEST_CASE(Test_Construction_WithoutHomeAssistant_NoLWT)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto client = integration.GetMqttHub()->GetMqttClient();
	BOOST_REQUIRE(client != nullptr);

	auto& will = client->GetWill();
	BOOST_CHECK(!will.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration lifecycle tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_Lifecycle)

BOOST_AUTO_TEST_CASE(Test_IsEnabled_WhenEnabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(integration.IsEnabled());
}

BOOST_AUTO_TEST_CASE(Test_IsEnabled_WhenDisabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(!integration.IsEnabled());
}

BOOST_AUTO_TEST_CASE(Test_IsRunning_WhenNotStarted)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	// Not started, not connected -> not running
	BOOST_CHECK(!integration.IsRunning());
}

BOOST_AUTO_TEST_CASE(Test_IsRunning_WhenDisabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(!integration.IsRunning());
}

BOOST_AUTO_TEST_CASE(Test_Start_WhenEnabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Start());

	integration.Stop();
}

BOOST_AUTO_TEST_CASE(Test_Start_WhenDisabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Start());
}

BOOST_AUTO_TEST_CASE(Test_Stop_WhenNotStarted_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Stop());
}

BOOST_AUTO_TEST_CASE(Test_Stop_WhenDisabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Stop());
}

BOOST_AUTO_TEST_CASE(Test_Poll_WhenEnabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Poll());
}

BOOST_AUTO_TEST_CASE(Test_Poll_WhenDisabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Poll());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration hub connection tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_HubConnections)

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_Individual_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	BOOST_CHECK_NO_THROW(integration.ConnectHubs(data_hub, equip_hub, stats_hub));
}

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_WithNullHubs_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.ConnectHubs(nullptr, nullptr, nullptr));
}

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_WhenDisabled_NoCrash)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	BOOST_CHECK_NO_THROW(integration.ConnectHubs(data_hub, equip_hub, stats_hub));
}

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_ViaHubLocator_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	Kernel::HubLocator locator;
	locator.Register(data_hub).Register(equip_hub).Register(stats_hub);

	BOOST_CHECK_NO_THROW(integration.ConnectHubs(locator));
}

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_ViaHubLocator_MissingHubs_DegradesGracefully)
{
	// An empty locator resolves every TryFind to null: the connect logs each missing capability as
	// degraded but must not throw, and the "device" command (which needs the dispatcher) is not
	// registered because ICommandDispatcher was not found.
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	Kernel::HubLocator locator;   // nothing registered
	BOOST_CHECK_NO_THROW(integration.ConnectHubs(locator));

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);
	// The default handlers still exist; RegisterDeviceCommand runs but with a null dispatcher the
	// command is still registered (dispatcher is locked at dispatch time), so we only assert no-throw
	// plus the still-present default commands.
	BOOST_CHECK(hub->HasCommand("status"));
	BOOST_CHECK(hub->HasCommand("refresh"));
}

BOOST_AUTO_TEST_CASE(Test_ConnectHubs_WithHomeAssistant_ConnectsDataHub)
{
	boost::asio::io_context ioc;
	auto settings = MakeHaEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

	// Should not throw — HA discovery should get the data hub too
	BOOST_CHECK_NO_THROW(integration.ConnectHubs(data_hub, equip_hub, stats_hub));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration default command handler tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_DefaultCommands)

BOOST_AUTO_TEST_CASE(Test_DefaultCommands_RegisteredWhenEnabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);

	// Should have "status" and "refresh" default commands before hubs are connected.
	BOOST_CHECK(hub->HasCommand("status"));
	BOOST_CHECK(hub->HasCommand("refresh"));
	BOOST_CHECK_GE(hub->CommandCount(), 2u);

	// The "device" command is NOT registered until the command dispatcher is available
	// (it used to be registered here as a dead placeholder that only echoed "acknowledged").
	BOOST_CHECK(!hub->HasCommand("device"));
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_RegisteredAfterHubsConnected)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);

	// Before connecting hubs, the "device" command handler should not exist.
	BOOST_CHECK(!hub->HasCommand("device"));

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();
	integration.ConnectHubs(data_hub, equip_hub, stats_hub);

	// After connecting hubs, the real "device" and "setpoint" handlers are registered.
	BOOST_CHECK(hub->HasCommand("device"));
	BOOST_CHECK(hub->HasCommand("setpoint"));
}

BOOST_AUTO_TEST_CASE(Test_DefaultCommands_NotRegisteredWhenDisabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	// No hub means no commands
	BOOST_CHECK(integration.GetMqttHub() == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration access tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_Access)

BOOST_AUTO_TEST_CASE(Test_GetMqttHub_WhenEnabled_ReturnsHub)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto hub = integration.GetMqttHub();
	BOOST_CHECK(hub != nullptr);
}

BOOST_AUTO_TEST_CASE(Test_GetMqttHub_WhenDisabled_ReturnsNull)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK(integration.GetMqttHub() == nullptr);
}

BOOST_AUTO_TEST_CASE(Test_GetMqttHub_ReturnsSameInstance)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto hub1 = integration.GetMqttHub();
	auto hub2 = integration.GetMqttHub();
	BOOST_CHECK_EQUAL(hub1.get(), hub2.get());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration factory function tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_Factory)

BOOST_AUTO_TEST_CASE(Test_CreateMqttIntegration_WhenEnabled_ReturnsInstance)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();

	auto integration = Mqtt::CreateMqttIntegration(ioc, settings);
	BOOST_CHECK(integration != nullptr);
	BOOST_CHECK(integration->IsEnabled());
}

BOOST_AUTO_TEST_CASE(Test_CreateMqttIntegration_WhenDisabled_ReturnsNull)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();

	auto integration = Mqtt::CreateMqttIntegration(ioc, settings);
	BOOST_CHECK(integration == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration full lifecycle (no-crash) tests
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_FullLifecycle)

BOOST_AUTO_TEST_CASE(Test_FullLifecycle_StartPollStop_WhenEnabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();
	integration.ConnectHubs(data_hub, equip_hub, stats_hub);

	BOOST_CHECK_NO_THROW(integration.Start());
	BOOST_CHECK_NO_THROW(integration.Poll());
	BOOST_CHECK_NO_THROW(integration.Stop());
}

BOOST_AUTO_TEST_CASE(Test_FullLifecycle_StartPollStop_WhenDisabled)
{
	boost::asio::io_context ioc;
	auto settings = MakeDisabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	BOOST_CHECK_NO_THROW(integration.Start());
	BOOST_CHECK_NO_THROW(integration.Poll());
	BOOST_CHECK_NO_THROW(integration.Stop());
}

BOOST_AUTO_TEST_CASE(Test_FullLifecycle_WithHomeAssistant)
{
	boost::asio::io_context ioc;
	auto settings = MakeHaEnabledSettings();
	Mqtt::MqttIntegration integration(ioc, settings);

	auto data_hub = std::make_shared<Kernel::DataHub>();
	auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
	auto stats_hub = std::make_shared<Kernel::StatisticsHub>();
	integration.ConnectHubs(data_hub, equip_hub, stats_hub);

	BOOST_CHECK_NO_THROW(integration.Start());
	BOOST_CHECK_NO_THROW(integration.Poll());
	BOOST_CHECK_NO_THROW(integration.Stop());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration dynamic heater-command tests
//
// Heaters are actuated through a dedicated heater/{slug} -> SetHeaterMode(body,
// on/off) route registered by RegisterDynamicDeviceCommands() (fired from the
// hub's OnDevicesPublished signal). These tests drive that path end to end:
// build a DataHub heater, connect a recording dispatcher via the locator, fire
// the signal, then push a command through the public HandleMessage() seam.
//=============================================================================

namespace
{
	namespace Traits = Kernel::AuxillaryTraitsTypes;

	// Recording dispatcher: captures heater commands so a test can prove the
	// heater/{slug} route maps the ON/OFF payload to the right body + enable flag.
	class RecordingHeaterDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		std::vector<std::pair<Kernel::BodyOfWaterIds, bool>> heater_calls;
		std::vector<std::uint8_t> pool_setpoints;
		std::vector<std::uint8_t> spa_setpoints;
		std::vector<std::pair<std::string, DeviceAction>> device_calls;
		std::vector<std::string> toggle_label_calls;
		std::vector<std::uint8_t> chlorinator_percentages;
		std::vector<bool> chlorinator_boosts;
		std::vector<Kernel::CirculationModes> circulation_modes;

		// When set, SetPoolSetpoint/SetSpaSetpoint return this instead of Success so the
		// failure-logging branch of the setpoint handler can be exercised.
		CommandResult setpoint_result{ CommandResult::Success };

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string& label) override { toggle_label_calls.push_back(label); return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string& label, DeviceAction action) override { device_calls.emplace_back(label, action); return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t value) override { pool_setpoints.push_back(value); return setpoint_result; }
		CommandResult SetSpaSetpoint(std::uint8_t value) override { spa_setpoints.push_back(value); return setpoint_result; }
		CommandResult SetChlorinatorPercentage(std::uint8_t pct) override { chlorinator_percentages.push_back(pct); return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool enable) override { chlorinator_boosts.push_back(enable); return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes mode) override { circulation_modes.push_back(mode); return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds body, bool enable) override { heater_calls.emplace_back(body, enable); return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct HeaterCommandFixture
	{
		boost::asio::io_context ioc;
		std::shared_ptr<RecordingHeaterDispatcher> dispatcher{ std::make_shared<RecordingHeaterDispatcher>() };
		std::shared_ptr<Kernel::DataHub> data_hub{ std::make_shared<Kernel::DataHub>() };
		std::shared_ptr<Kernel::EquipmentHub> equip_hub{ std::make_shared<Kernel::EquipmentHub>() };
		std::shared_ptr<Kernel::StatisticsHub> stats_hub{ std::make_shared<Kernel::StatisticsHub>() };
		Mqtt::MqttIntegration integration;

		HeaterCommandFixture()
			: integration(ioc, MakeHaEnabledSettings())
		{
		}

		~HeaterCommandFixture()
		{
			integration.Stop();
		}

		// Add a heater AuxillaryDevice to the DataHub so DataHub::Heaters() returns it.
		// with_body=false omits the BodyOfWaterTrait to exercise the skip path.
		void AddHeater(const std::string& label, Kernel::BodyOfWaterIds body, bool with_body = true)
		{
			auto heater = std::make_shared<Kernel::AuxillaryDevice>();
			heater->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Heater);
			heater->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
			if (with_body)
			{
				heater->AuxillaryTraits.Set(Traits::BodyOfWaterTrait{}, body);
			}
			data_hub->Devices.Add(heater);
		}

		// Add a labelled auxillary device so DataHub::Auxillaries() returns it and the
		// dynamic device/{slug} -> CommandByLabel route is registered for it.
		void AddAux(const std::string& label)
		{
			auto aux = std::make_shared<Kernel::AuxillaryDevice>();
			aux->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
			aux->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
			data_hub->Devices.Add(aux);
		}

		// Connect hubs + dispatcher via the locator (the only overload that wires the
		// command dispatcher), start (connects OnDevicesPublished -> dynamic command
		// registration, HA enabled), then fire the signal to run the registration.
		Mqtt::MqttHub& ConnectAndPublish()
		{
			Kernel::HubLocator locator;
			locator.Register(data_hub).Register(equip_hub).Register(stats_hub)
				.Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
			integration.ConnectHubs(locator);
			integration.Start();

			auto hub = integration.GetMqttHub();
			BOOST_REQUIRE(hub != nullptr);
			hub->OnDevicesPublished();
			return *hub;
		}
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_HeaterCommands, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_RegisteredForLabelledHeaterWithBody)
{
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool);
	auto& hub = ConnectAndPublish();

	BOOST_CHECK(hub.HasCommand("heater/pool_heater"));
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_On_DispatchesEnable)
{
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("heater/pool_heater"));

	// Inject the inbound command via the client's public OnMessageReceived signal,
	// which the hub subscribes to and routes through to the registered handler.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("heater/pool_heater"), "ON");

	BOOST_REQUIRE_EQUAL(dispatcher->heater_calls.size(), 1u);
	BOOST_CHECK(dispatcher->heater_calls[0].first == Kernel::BodyOfWaterIds::Pool);
	BOOST_CHECK_EQUAL(dispatcher->heater_calls[0].second, true);
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_Off_DispatchesDisable)
{
	AddHeater("Spa Heater", Kernel::BodyOfWaterIds::Spa);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("heater/spa_heater"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("heater/spa_heater"), "OFF");

	BOOST_REQUIRE_EQUAL(dispatcher->heater_calls.size(), 1u);
	BOOST_CHECK(dispatcher->heater_calls[0].first == Kernel::BodyOfWaterIds::Spa);
	BOOST_CHECK_EQUAL(dispatcher->heater_calls[0].second, false);
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_UnknownPayload_NotDispatched)
{
	AddHeater("Pool Heater", Kernel::BodyOfWaterIds::Pool);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("heater/pool_heater"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("heater/pool_heater"), "MAYBE");

	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_HeaterCommand_NotRegistered_WhenBodyTraitMissing)
{
	AddHeater("Orphan Heater", Kernel::BodyOfWaterIds::Pool, /*with_body=*/false);
	auto& hub = ConnectAndPublish();

	BOOST_CHECK(!hub.HasCommand("heater/orphan_heater"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration setpoint-command tests
//
// The "setpoint" command takes {"target":"pool"|"spa","temperature":<celsius>}.
// The temperature is converted from Celsius to the DataHub's system units (the
// wire domain) and clamped to uint8_t before dispatch to SetPool/SetSpaSetpoint.
// The command is registered by ConnectHubs(), so ConnectAndPublish() is enough.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_SetpointCommands, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_Setpoint_Pool_Fahrenheit_ConvertsCelsiusToWire)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Fahrenheit);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint"));

	// 25C -> round(25 * 9/5 + 32) = 77F on the wire.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"pool","temperature":25.0})");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 77u);
	BOOST_CHECK(dispatcher->spa_setpoints.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_Spa_Celsius_PassesThroughRounded)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint"));

	// Celsius system units: the value is used directly (rounded), no F conversion.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"spa","temperature":39.0})");

	BOOST_REQUIRE_EQUAL(dispatcher->spa_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->spa_setpoints[0], 39u);
	BOOST_CHECK(dispatcher->pool_setpoints.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_UnknownTarget_NotDispatched)
{
	auto& hub = ConnectAndPublish();

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"deck","temperature":25.0})");

	BOOST_CHECK(dispatcher->pool_setpoints.empty());
	BOOST_CHECK(dispatcher->spa_setpoints.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_NonPositiveTemperature_NotDispatched)
{
	auto& hub = ConnectAndPublish();

	// temperature <= 0 is rejected by the handler before any dispatch.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"pool","temperature":0.0})");
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"pool","temperature":-3.0})");

	BOOST_CHECK(dispatcher->pool_setpoints.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_OutOfRange_ClampedToWireMax)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Fahrenheit);
	auto& hub = ConnectAndPublish();

	// 500C converts well past 255F and must clamp to the uint8_t wire maximum.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"pool","temperature":500.0})");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 255u);
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_HaPlainNumberTopic_Dispatches)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint/pool"));

	// The HA number-entity topic carries a bare number (Celsius), not a JSON object.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint/pool"), "30");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 30u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration dynamic device-command tests
//
// Auxillary devices on the DataHub get a device/{slug} -> CommandByLabel route
// registered from the hub's OnDevicesPublished signal. ON/OFF map to On/Off; an
// unknown payload is dropped; and two labels that Slugify to the same key share
// a single command (the first claimant wins) rather than silently shadowing.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_DeviceCommands, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_RegisteredForLabelledAux)
{
	AddAux("Pool Light");
	auto& hub = ConnectAndPublish();

	BOOST_CHECK(hub.HasCommand("device/pool_light"));
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_On_DispatchesCommandByLabel)
{
	AddAux("Pool Light");
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("device/pool_light"), "ON");

	BOOST_REQUIRE_EQUAL(dispatcher->device_calls.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->device_calls[0].first, "Pool Light");
	BOOST_CHECK(dispatcher->device_calls[0].second == Interfaces::ICommandDispatcher::DeviceAction::On);
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_UnknownPayload_NotDispatched)
{
	AddAux("Pool Light");
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("device/pool_light"), "MAYBE");

	BOOST_CHECK(dispatcher->device_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_SlugCollision_RegistersSingleHandler)
{
	// Two distinct labels that Slugify to the same key ("pool_light") must share one
	// command: the first claimant wins and the second is skipped, so firing the topic
	// dispatches exactly once (never a double registration / silent shadow).
	AddAux("Pool Light");
	AddAux("POOL LIGHT");
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device/pool_light"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("device/pool_light"), "OFF");

	BOOST_REQUIRE_EQUAL(dispatcher->device_calls.size(), 1u);
	BOOST_CHECK(dispatcher->device_calls[0].second == Interfaces::ICommandDispatcher::DeviceAction::Off);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration generic "device" command + chlorinator/circulation commands.
//
// The generic "device" command (registered by ConnectHubs) takes a JSON
// {"device_id": ..., "action": ...} and routes to ToggleByUuid / ToggleByLabel.
// The chlorinator + circulation commands are registered by the dynamic-command
// pass (fired via OnDevicesPublished) independent of any device being present.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_ControlCommands, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_GenericDeviceCommand_NonUuidId_RoutesToToggleByLabel)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device"));

	// A non-UUID device_id falls back to ToggleByLabel.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("device"), R"({"device_id":"Pool Light","action":"toggle"})");

	BOOST_REQUIRE_EQUAL(dispatcher->toggle_label_calls.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->toggle_label_calls[0], "Pool Light");
}

BOOST_AUTO_TEST_CASE(Test_GenericDeviceCommand_MissingDeviceId_NotDispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("device"));

	// An empty device_id is rejected before any dispatch.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("device"), R"({"action":"toggle"})");

	BOOST_CHECK(dispatcher->toggle_label_calls.empty());
	BOOST_CHECK(dispatcher->device_calls.empty());
}

BOOST_AUTO_TEST_CASE(Test_ChlorinatorPercentage_Dispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("chlorinator/percentage"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("chlorinator/percentage"), "60");

	BOOST_REQUIRE_EQUAL(dispatcher->chlorinator_percentages.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->chlorinator_percentages[0], 60u);
}

BOOST_AUTO_TEST_CASE(Test_ChlorinatorBoost_On_Dispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("chlorinator/boost"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("chlorinator/boost"), "ON");

	BOOST_REQUIRE_EQUAL(dispatcher->chlorinator_boosts.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->chlorinator_boosts[0], true);
}

BOOST_AUTO_TEST_CASE(Test_CirculationMode_Spa_Dispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("circulation/mode"));

	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("circulation/mode"), "spa");

	BOOST_REQUIRE_EQUAL(dispatcher->circulation_modes.size(), 1u);
	BOOST_CHECK(dispatcher->circulation_modes[0] == Kernel::CirculationModes::Spa);
}

BOOST_AUTO_TEST_CASE(Test_CirculationMode_Unknown_NotDispatched)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("circulation/mode"));

	// An unrecognised mode string is rejected before any dispatch.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("circulation/mode"), "sideways");

	BOOST_CHECK(dispatcher->circulation_modes.empty());
}

BOOST_AUTO_TEST_CASE(Test_HaSetpoint_NonPositiveValue_Rejected)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint/pool"));

	// The HA number-entity handler rejects a non-positive value before dispatch.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint/pool"), "0");

	BOOST_CHECK(dispatcher->pool_setpoints.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoint_DispatchFailure_TakesFailureBranch)
{
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	dispatcher->setpoint_result = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("setpoint"));

	// The dispatcher reports a failure: the handler still dispatched (recording the value) and
	// takes the failure-logging branch rather than the success branch.
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("setpoint"), R"({"target":"pool","temperature":30.0})");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 30u);
}

BOOST_AUTO_TEST_CASE(Test_HaSetpoint_FahrenheitPreference_ConvertsToCelsiusBeforeDispatch)
{
	// The HA number-entity handler reads the display-units preference at command time: with a
	// Fahrenheit preference the inbound value is normalised (F -> C) before dispatch_setpoint,
	// which (system units Celsius here) then rounds it straight onto the wire.
	data_hub->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);
	auto prefs = std::make_shared<Kernel::PreferencesHub>();
	prefs->Temperature_DisplayUnits = Kernel::TemperatureUnits::Fahrenheit;

	Kernel::HubLocator locator;
	locator.Register(data_hub).Register(equip_hub).Register(stats_hub).Register(prefs)
		.Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
	integration.ConnectHubs(locator);
	integration.Start();
	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);
	hub->OnDevicesPublished();
	BOOST_REQUIRE(hub->HasCommand("setpoint/pool"));

	// 86 F -> (86 - 32) * 5/9 = 30 C -> rounded 30 on the wire (Celsius system units).
	hub->GetMqttClient()->OnMessageReceived(hub->CommandTopic("setpoint/pool"), "86");

	BOOST_REQUIRE_EQUAL(dispatcher->pool_setpoints.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->pool_setpoints[0], 30u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Default "status"/"refresh" command bodies + the dispatcher-unavailable arms of
// the generic device and setpoint commands. These fire the registered handlers
// through the public OnMessageReceived seam and assert the observable effect.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_DefaultCommandBodies, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_StatusCommand_RepublishesAllStatus)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("status"));

	// A connected hub is required for PublishAllStatus() to actually enqueue; force it.
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("status"), "");

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	bool saw_system_status = false;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/system/status") != std::string::npos) { saw_system_status = true; }
	}
	BOOST_CHECK_MESSAGE(saw_system_status, "the status command should republish all status");
}

BOOST_AUTO_TEST_CASE(Test_RefreshCommand_PublishesResponse)
{
	auto& hub = ConnectAndPublish();
	BOOST_REQUIRE(hub.HasCommand("refresh"));

	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());
	hub.GetMqttClient()->OnMessageReceived(hub.CommandTopic("refresh"), "");

	// The refresh handler republishes status AND publishes a response/refresh envelope.
	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	bool saw_response = false;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/response/refresh") != std::string::npos)
		{
			saw_response = true;
			auto body = nlohmann::json::parse(pending.payload);
			BOOST_CHECK_EQUAL(body.value("command", ""), "refresh");
			BOOST_CHECK_EQUAL(body.value("status", ""), "completed");
		}
	}
	BOOST_CHECK_MESSAGE(saw_response, "the refresh command should publish a response/refresh envelope");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Dispatcher-unavailable arms: when the command dispatcher is not resolvable
// (empty locator), the generic "device" and "setpoint" handlers take their
// null-dispatcher early-return branch and dispatch nothing.
//=============================================================================

//=============================================================================
// Home-Assistant seed wiring: with HA enabled, the client's OnConnected drives
// the discovery seed (subscribe + arm the grace window + publish online/states),
// a replayed retained config adopts+republishes, and an elapsed grace deadline in
// Poll() publishes discovery when no retained config arrived.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_MqttIntegration_HaSeed, HeaterCommandFixture)

BOOST_AUTO_TEST_CASE(Test_HaOnConnected_PublishesOnlineAndArmsSeed)
{
	auto& hub = ConnectAndPublish();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// Fire the client's OnConnected exactly as a real broker connect would: the integration's HA
	// connected lambda publishes availability "online", subscribes to the retained config topic and
	// publishes the device states.
	hub.GetMqttClient()->OnConnected();

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	bool saw_online = false;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/status/availability") != std::string::npos && pending.payload == "online")
		{
			saw_online = true;
		}
	}
	BOOST_CHECK_MESSAGE(saw_online, "HA OnConnected should publish availability online");
}

BOOST_AUTO_TEST_CASE(Test_HaSeed_RetainedConfigReplay_AdoptsAndRepublishes)
{
	auto& hub = ConnectAndPublish();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// Arm the seed window (sets m_HaSeedPending + subscribes to the config topic).
	hub.GetMqttClient()->OnConnected();

	// The broker replays the retained discovery config on the seed topic: the seed lambda adopts the
	// components and republishes a fresh discovery config to the same topic.
	const std::string config_topic = "homeassistant/device/aqualink_test/config";
	nlohmann::json retained;
	retained["cmps"]["aux_ghost"] = { {"p", "switch"}, {"name", "Ghost"} };
	hub.GetMqttClient()->OnMessageReceived(config_topic, retained.dump());

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
	bool republished = false;
	for (const auto& pending : queue)
	{
		if (pending.topic == config_topic && !pending.payload.empty())
		{
			auto body = nlohmann::json::parse(pending.payload);
			if (body.contains("cmps")) { republished = true; }
		}
	}
	BOOST_CHECK_MESSAGE(republished, "adopting the retained config should trigger a discovery republish");
}

BOOST_AUTO_TEST_CASE(Test_HaSeed_GraceElapsed_PollPublishesDeferredDiscovery)
{
	// Drive the seed-grace deadline deterministically via the integration's
	// injectable monotonic clock (no real wait against HA_SEED_GRACE).
	auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
	integration.SetSteadyClock([&] { return fake_now; });

	auto& hub = ConnectAndPublish();
	Test::MqttClientPacketTest::ForceConnectedState(*hub.GetMqttClient());

	// Arm the seed window: OnConnected sets m_HaSeedPending and the deadline = now + grace.
	hub.GetMqttClient()->OnConnected();

	const std::string config_topic = "homeassistant/device/aqualink_test/config";
	auto count_config_publishes = [&]
	{
		auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub.GetMqttClient());
		std::size_t n = 0;
		for (const auto& pending : queue)
		{
			if (pending.topic == config_topic && !pending.payload.empty()) { ++n; }
		}
		return n;
	};

	// A pre-deadline Poll settles the queue (the hub flush drains prior publishes) and must NOT
	// fire the deferred discovery config; capture that settled baseline.
	integration.Poll();
	const auto before = count_config_publishes();

	// Advance past HA_SEED_GRACE: with no retained config ever replayed (fresh broker), the next
	// Poll() publishes the deferred discovery config so HA entities are not held back indefinitely.
	fake_now += std::chrono::seconds(4);
	integration.Poll();
	BOOST_CHECK_GT(count_config_publishes(), before);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_NoDispatcher)

BOOST_AUTO_TEST_CASE(Test_DeviceCommand_NoDispatcher_TakesUnavailableBranch)
{
	boost::asio::io_context ioc;
	Mqtt::MqttIntegration integration(ioc, MakeHaEnabledSettings());

	Kernel::HubLocator locator;   // nothing registered -> no ICommandDispatcher
	integration.ConnectHubs(locator);
	integration.Start();

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);
	BOOST_REQUIRE(hub->HasCommand("device"));
	Test::MqttClientPacketTest::ForceConnectedState(*hub->GetMqttClient());

	// The handler locks a null dispatcher, logs "not available", publishes an error response, and
	// returns before any device_id parsing. No throw is the primary observable.
	BOOST_CHECK_NO_THROW(hub->GetMqttClient()->OnMessageReceived(
		hub->CommandTopic("device"), R"({"device_id":"Pool Light","action":"toggle"})"));

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub->GetMqttClient());
	bool saw_error = false;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/response/device") != std::string::npos)
		{
			auto body = nlohmann::json::parse(pending.payload);
			if (body.value("status", "") == "error") { saw_error = true; }
		}
	}
	BOOST_CHECK_MESSAGE(saw_error, "the null-dispatcher device command should publish an error response");

	integration.Stop();
}

BOOST_AUTO_TEST_CASE(Test_SetpointCommand_NoDispatcher_ReturnsErrorStatus)
{
	boost::asio::io_context ioc;
	Mqtt::MqttIntegration integration(ioc, MakeHaEnabledSettings());

	Kernel::HubLocator locator;   // no dispatcher
	integration.ConnectHubs(locator);
	integration.Start();

	auto hub = integration.GetMqttHub();
	BOOST_REQUIRE(hub != nullptr);
	BOOST_REQUIRE(hub->HasCommand("setpoint"));
	Test::MqttClientPacketTest::ForceConnectedState(*hub->GetMqttClient());

	// dispatch_setpoint locks a null dispatcher first and returns "error" before target validation.
	BOOST_CHECK_NO_THROW(hub->GetMqttClient()->OnMessageReceived(
		hub->CommandTopic("setpoint"), R"({"target":"pool","temperature":28.0})"));

	auto& queue = Test::MqttClientPacketTest::GetPublishQueue(*hub->GetMqttClient());
	bool saw_error = false;
	for (const auto& pending : queue)
	{
		if (pending.topic.find("/response/setpoint") != std::string::npos)
		{
			auto body = nlohmann::json::parse(pending.payload);
			if (body.value("status", "") == "error") { saw_error = true; }
		}
	}
	BOOST_CHECK_MESSAGE(saw_error, "the null-dispatcher setpoint command should report an error status");

	integration.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// MqttIntegration lifetime / no-leak regression tests
//
// Regression for a command-handler self-reference cycle: the MQTT command
// handlers registered on the hub used to capture a shared_ptr<MqttHub> (the hub
// itself) and were then stored in that same hub's m_CommandHandlers map. The
// resulting hub -> handler -> hub cycle kept the hub (and its MqttClient, and
// every command-topic string) alive to process exit, which surfaced as the CRT
// "Detected memory leaks!" dump on the debug test binary. The handlers now
// capture a weak_ptr, so the hub is released once its owning MqttIntegration is
// destroyed. These tests fail (weak_ptrs still valid) against the buggy code.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttIntegration_Lifetime)

BOOST_AUTO_TEST_CASE(Test_Integration_ReleasesHubAndClient_AfterDestruction)
{
	std::weak_ptr<Mqtt::MqttHub> weak_hub;
	std::weak_ptr<Mqtt::MqttClient> weak_client;

	{
		boost::asio::io_context ioc;
		Mqtt::MqttIntegration integration(ioc, MakeHaEnabledSettings());

		auto data_hub = std::make_shared<Kernel::DataHub>();
		auto equip_hub = std::make_shared<Kernel::EquipmentHub>();
		auto stats_hub = std::make_shared<Kernel::StatisticsHub>();

		// A labelled aux so the dynamic device/{slug} command-registration path also runs.
		namespace Traits = Kernel::AuxillaryTraitsTypes;
		auto aux = std::make_shared<Kernel::AuxillaryDevice>();
		aux->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		aux->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Pool Light" });
		data_hub->Devices.Add(aux);

		// ConnectHubs registers the "device"/"setpoint" handlers; the ctor already
		// registered "status"/"refresh"; Start() + OnDevicesPublished() drive the
		// dynamic "device/pool_light" registration. All four paths captured the hub.
		Kernel::HubLocator locator;
		locator.Register(data_hub).Register(equip_hub).Register(stats_hub);
		integration.ConnectHubs(locator);

		auto hub = integration.GetMqttHub();
		BOOST_REQUIRE(hub != nullptr);
		weak_hub = hub;
		weak_client = hub->GetMqttClient();

		integration.Start();
		hub->OnDevicesPublished();
		BOOST_REQUIRE(hub->HasCommand("status"));
		BOOST_REQUIRE(hub->HasCommand("device"));
		BOOST_REQUIRE(hub->HasCommand("setpoint"));
		BOOST_REQUIRE(hub->HasCommand("device/pool_light"));
		integration.Stop();
	}

	// Once the owning MqttIntegration is gone (and the local io_context has been
	// destroyed, unwinding any pending async work that transiently holds the
	// client), no command handler should keep the hub or its client alive. A
	// surviving reference here is the retain cycle that produced the CRT leak dump.
	BOOST_CHECK(weak_hub.expired());
	BOOST_CHECK(weak_client.expired());
}

BOOST_AUTO_TEST_SUITE_END()
