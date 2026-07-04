#include <memory>
#include <span>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "equipment/jandy_equipment.h"
#include "interfaces/idevice.h"
#include "jandy/jandy.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/jandy_emulated_device_types.h"
#include "jandy/options/options_jandy.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/statistics_hub.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_ids.h"
#include "options/options_developer_options.h"
#include "options/options_settings.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;

namespace
{
	// Deserialise a framed+checksummed Jandy ACK addressed to `destination`,
	// then fire it through JandyMessage_Ack's static signal so any connected
	// JandyEquipment slot (wired via the variadic ConnectIdentify fold) runs
	// IdentifyAndAddDevice exactly as it would on the wire.
	void EmitAckTo(uint8_t destination)
	{
		// Two payload bytes => the frame is long enough for Ack to deserialise
		// (it reads indices 4 and 5), which is what sets the message Destination.
		const auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(destination, 0x01, { 0x00, 0x00 });

		Messages::JandyMessage_Ack ack;
		const auto byte_span = std::as_bytes(std::span<const uint8_t>(frame.data(), frame.size()));
		BOOST_REQUIRE(ack.Deserialize(byte_span));
		BOOST_REQUIRE_EQUAL(static_cast<unsigned>(ack.Destination().Id()()), static_cast<unsigned>(destination));

		(*Messages::JandyMessage_Ack::GetSignal())(ack);
	}

	std::size_t DeviceCount(const Kernel::EquipmentHub& hub)
	{
		std::size_t count = 0;
		hub.ForEachDevice([&count](const Interfaces::IDevice&) { ++count; });
		return count;
	}
}

BOOST_AUTO_TEST_SUITE(JandyEquipment_TestSuite)

// =============================================================================
// Dispatch: a supported destination class creates exactly one device and a
// duplicate destination is not re-added (exercises the variadic ConnectIdentify
// fold + IdentifyAndAddDevice + the single-lookup stats path).
// =============================================================================

BOOST_AUTO_TEST_CASE(SupportedClass_AddsDeviceOnce_DuplicateNotReadded)
{
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
	auto stats_hub = hub_locator.Find<Kernel::StatisticsHub>();

	Equipment::JandyEquipment equipment(hub_locator);

	BOOST_REQUIRE_EQUAL(DeviceCount(*equipment_hub), 0U);

	// 0x38 is an LX_Heater id -> one HeaterDevice is created.
	EmitAckTo(0x38);
	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 1U);

	// Same destination again -> DeviceExists short-circuits, no second device.
	EmitAckTo(0x38);
	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 1U);

	// Both ACKs were still counted in the per-id statistics.
	BOOST_CHECK_EQUAL(stats_hub->MessageCounts[Messages::JandyMessageIds::Ack].Count(), 2U);
}

// =============================================================================
// Unsupported device classes are tolerated (no device created) and, with the
// once-per-class rate limiting, repeated messages to the same unsupported class
// do not create devices or crash.
// =============================================================================

BOOST_AUTO_TEST_CASE(UnsupportedClass_CreatesNoDevice_AndIsRateLimited)
{
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
	auto stats_hub = hub_locator.Find<Kernel::StatisticsHub>();

	Equipment::JandyEquipment equipment(hub_locator);

	// 0x84 / 0x85 are Chem_Analyzer ids -> recognised at the id layer but with no device
	// case in IdentifyAndAddDevice, so no device is created. (SpaRemote 0x20 is no longer a
	// valid example here -- it is now recognised as a spaside-remote keypad.)
	EmitAckTo(0x84);
	EmitAckTo(0x85);

	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 0U);
	BOOST_CHECK_EQUAL(stats_hub->MessageCounts[Messages::JandyMessageIds::Ack].Count(), 2U);
}

// =============================================================================
// Spaside remotes are recognised: "Dual Spa Switch" (2x4rem, 0x10) and "Spa Link"
// (8button, 0x20) are spa-side keypads and each gets a device on the bus.
// =============================================================================

BOOST_AUTO_TEST_CASE(SpasideRemotes_AreRecognisedAsDevices)
{
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();

	Equipment::JandyEquipment equipment(hub_locator);

	EmitAckTo(0x10);   // Dual Spa Switch (2x4rem)
	EmitAckTo(0x20);   // Spa Link (8button)

	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 2U);
}

// =============================================================================
// Lifetime: JandyEquipment holds a NON-owning raw pointer to the EquipmentHub
// (no shared_ptr back-edge). Once the equipment and the owning HubLocator go
// out of scope, a weak_ptr to the hub must expire -- proving there is no
// reference cycle keeping the hub (and, transitively, the equipment/devices)
// alive at shutdown.
// =============================================================================

BOOST_AUTO_TEST_CASE(NoReferenceCycle_HubExpiresAfterScope)
{
	std::weak_ptr<Kernel::EquipmentHub> hub_observer;

	{
		Test::HubLocatorInjector hub_locator;
		auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
		hub_observer = equipment_hub;

		Equipment::JandyEquipment equipment(hub_locator);

		// Drive a message so the equipment actually touches the hub via its raw
		// pointer (a shared_ptr back-edge, if reintroduced, would be taken here).
		EmitAckTo(0x38);
		BOOST_REQUIRE(!hub_observer.expired());

		// Release the local strong reference; the locator still owns the hub.
		equipment_hub.reset();
		BOOST_REQUIRE(!hub_observer.expired());
	}

	// Equipment + locator destroyed: nothing should still reference the hub.
	BOOST_CHECK(hub_observer.expired());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Jandy::Configure -- the top-level equipment wiring driven from parsed Settings.
// Adds the JandyEquipment to the hub, then (depending on flags) either defers to
// the auto-startup coordinator, disables emulation, or stands up the statically
// CLI-configured emulated-device set. These tests build a Settings map directly
// (bypassing program_options) and assert the resulting hub / DataHub state.
// =============================================================================

BOOST_AUTO_TEST_SUITE(JandyConfigure_TestSuite)

namespace
{
	// A Settings with the given JandySettings and (optionally) DeveloperSettings so
	// Jandy::Configure can look both areas up by name.
	Options::Settings MakeSettings(const Jandy::Options::JandySettings& jandy,
		const Options::Developer::DeveloperSettings& developer = {})
	{
		Options::Settings settings;
		settings.Set(Jandy::Options::JandySettings::AreaName(), jandy);
		settings.Set(Options::Developer::DeveloperSettings::AreaName(), developer);
		return settings;
	}
}

BOOST_AUTO_TEST_CASE(Configure_MissingJandySettings_IsANoOp)
{
	// No Jandy area in the settings map -> Configure logs an error and returns before touching the
	// EquipmentHub (no JandyEquipment registered).
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();

	Options::Settings settings;   // empty -- no Jandy area

	Jandy::Configure(hub_locator, settings);

	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 0U);
}

BOOST_AUTO_TEST_CASE(Configure_DefaultSettings_AddsEquipmentButNoStaticDevices)
{
	// Defaults: emulation enabled, auto-startup off, but no emulated_devices configured -> the
	// JandyEquipment is registered and the static device loop stands nothing up.
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
	auto data_hub = hub_locator.Find<Kernel::DataHub>();

	Jandy::Options::JandySettings jandy;   // all defaults
	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	// No emulated devices, and neither disable flag was set.
	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 0U);
	BOOST_CHECK(!data_hub->EmulationDisabled);
	BOOST_CHECK(!data_hub->PresenceGatingDisabled);
}

BOOST_AUTO_TEST_CASE(Configure_DecodeToMaster_IsWiredFromDeveloperSettings)
{
	// The developer decode-to-master flag flows through into Configure's observe-only wiring path
	// (the branch that logs and passes decode_to_master into JandyEquipment). Configure must still
	// complete cleanly with the flag on.
	Test::HubLocatorInjector hub_locator;

	Jandy::Options::JandySettings jandy;
	Options::Developer::DeveloperSettings developer;
	developer.decode_to_master_enabled = true;

	auto settings = MakeSettings(jandy, developer);

	BOOST_CHECK_NO_THROW(Jandy::Configure(hub_locator, settings));
}

BOOST_AUTO_TEST_CASE(Configure_DisableEmulation_SetsDataHubFlag_AndSkipsStaticDevices)
{
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
	auto data_hub = hub_locator.Find<Kernel::DataHub>();

	Jandy::Options::JandySettings jandy;
	jandy.disable_emulation = true;
	// Even with configured emulated devices, disable_emulation short-circuits the static loop.
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::OneTouch,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x41)));

	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	BOOST_CHECK(data_hub->EmulationDisabled);
	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 0U);   // static device loop skipped
}

BOOST_AUTO_TEST_CASE(Configure_DisablePresenceGating_SetsDataHubFlag)
{
	Test::HubLocatorInjector hub_locator;
	auto data_hub = hub_locator.Find<Kernel::DataHub>();

	Jandy::Options::JandySettings jandy;
	jandy.disable_presence_gating = true;

	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	BOOST_CHECK(data_hub->PresenceGatingDisabled);
}

BOOST_AUTO_TEST_CASE(Configure_AutoStartup_DefersStaticDeviceSelection)
{
	// Auto-startup on: the static, CLI-configured device set is skipped (the coordinator wired on
	// the io_context stands the emulation up dynamically instead). Any configured emulated_devices
	// are ignored here.
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();

	Jandy::Options::JandySettings jandy;
	jandy.auto_startup = true;
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::IAQ,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x33)));

	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 0U);   // deferred to the coordinator
}

BOOST_AUTO_TEST_CASE(Configure_StaticEmulatedDevices_StandsUpEachConfiguredType)
{
	// The static path: with emulation enabled and auto-startup off, each configured
	// (controller_type, device_type) pair stands a device up in the hub. Exercises the OneTouch
	// (setpoint-refresh) and IAQ switch arms.
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();

	Jandy::Options::JandySettings jandy;
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::OneTouch,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x41)));
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::IAQ,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x33)));

	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 2U);
}

BOOST_AUTO_TEST_CASE(Configure_StaticEmulatedDevices_CoversEveryControllerTypeArm)
{
	// Drive each switch arm in Configure's static loop -- Keypad, PDA, SerialAdapter, Spaside --
	// plus the Unknown default (which logs a warning and creates no device).
	Test::HubLocatorInjector hub_locator;
	auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();

	Jandy::Options::JandySettings jandy;
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::RS_Keypad,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x08)));
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::PDA,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x60)));
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::SerialAdapter,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x48)));
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::SpasideRemote,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0x10)));
	jandy.emulated_devices.emplace_back(Devices::JandyEmulatedDeviceTypes::Unknown,
		Devices::JandyDeviceType(Devices::JandyDeviceId(0xFF)));

	auto settings = MakeSettings(jandy);

	Jandy::Configure(hub_locator, settings);

	// Four concrete types create a device; Unknown creates none.
	BOOST_CHECK_EQUAL(DeviceCount(*equipment_hub), 4U);
}

BOOST_AUTO_TEST_SUITE_END()
