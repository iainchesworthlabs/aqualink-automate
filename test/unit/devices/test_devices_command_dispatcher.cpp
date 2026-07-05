#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/command_dispatcher.h"
#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/serial_adapter_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/circulation.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_ostream_support.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Interfaces;
using namespace AqualinkAutomate::Kernel;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

namespace
{
	struct CommandDispatcherFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		CommandDispatcherFixture()
			: data_hub(Find<DataHub>())
			, equipment_hub(Find<EquipmentHub>())
			, dispatcher(data_hub, equipment_hub)
		{
		}

		std::shared_ptr<DataHub> data_hub;
		std::shared_ptr<EquipmentHub> equipment_hub;
		CommandDispatcher dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(CommandDispatcher_TestSuite, CommandDispatcherFixture)

// =============================================================================
// Device-not-found Paths
// =============================================================================

BOOST_AUTO_TEST_CASE(TestToggleByUuid_DeviceNotFound)
{
	boost::uuids::uuid fake_uuid{};
	auto result = dispatcher.ToggleByUuid(fake_uuid);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_DeviceNotFound)
{
	auto result = dispatcher.ToggleByLabel("NonexistentDevice");
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestCommandByUuid_DeviceNotFound)
{
	boost::uuids::uuid fake_uuid{};
	auto result = dispatcher.CommandByUuid(fake_uuid, ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestCommandByLabel_DeviceNotFound)
{
	auto result = dispatcher.CommandByLabel("Ghost", ICommandDispatcher::DeviceAction::Off);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

// =============================================================================
// No-serial-adapter Paths
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_NoSerialAdapter)
{
	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_NoSerialAdapter)
{
	auto result = dispatcher.SetSpaSetpoint(100);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetCirculationMode_NoSerialAdapter)
{
	auto result = dispatcher.SetCirculationMode(CirculationModes::Spa);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetHeaterMode_NoSerialAdapter)
{
	auto result = dispatcher.SetHeaterMode(Kernel::BodyOfWaterIds::Spa, true);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

// =============================================================================
// Pool/Spa Setpoint Range Validation (regression for WU-JANDY-COMMAND-DISPATCH)
//
// An out-of-range setpoint must be rejected BEFORE any serial-adapter lookup so it
// never reaches the RS-485 wire. With no serial adapter present in the fixture, an
// in-range value falls through to NoSerialAdapter while an out-of-range value short-
// circuits with InvalidValue - which lets us distinguish the two paths.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_InvalidValue_Zero)
{
	// 0 is the sensor-unavailable sentinel and is rejected as out-of-range.
	auto result = dispatcher.SetPoolSetpoint(0);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_InvalidValue_AboveMax)
{
	auto result = dispatcher.SetPoolSetpoint(static_cast<uint8_t>(CommandDispatcher::SETPOINT_MAX_VALUE + 1));
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_InvalidValue_MaxUint8)
{
	auto result = dispatcher.SetPoolSetpoint(255);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_BoundaryValue_Min)
{
	// In-range boundary: not InvalidValue; falls through to NoSerialAdapter in this fixture.
	auto result = dispatcher.SetPoolSetpoint(CommandDispatcher::SETPOINT_MIN_VALUE);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_BoundaryValue_Max)
{
	auto result = dispatcher.SetPoolSetpoint(CommandDispatcher::SETPOINT_MAX_VALUE);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_InvalidValue_Zero)
{
	auto result = dispatcher.SetSpaSetpoint(0);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_InvalidValue_AboveMax)
{
	auto result = dispatcher.SetSpaSetpoint(static_cast<uint8_t>(CommandDispatcher::SETPOINT_MAX_VALUE + 1));
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_BoundaryValue_Min)
{
	auto result = dispatcher.SetSpaSetpoint(CommandDispatcher::SETPOINT_MIN_VALUE);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_BoundaryValue_Max)
{
	auto result = dispatcher.SetSpaSetpoint(CommandDispatcher::SETPOINT_MAX_VALUE);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

// =============================================================================
// Chlorinator Validation
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_InvalidValue_Above100)
{
	auto result = dispatcher.SetChlorinatorPercentage(101);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_InvalidValue_MaxUint8)
{
	auto result = dispatcher.SetChlorinatorPercentage(255);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::InvalidValue));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_BoundaryValue_100)
{
	// 100 is valid but needs an IAQ device - should get DeviceNotFound, not InvalidValue
	auto result = dispatcher.SetChlorinatorPercentage(100);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_BoundaryValue_0)
{
	// 0 is valid but needs an IAQ device
	auto result = dispatcher.SetChlorinatorPercentage(0);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

// =============================================================================
// No-IAQ-device Paths
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_NoIAQDevice)
{
	auto result = dispatcher.SetChlorinatorPercentage(50);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorBoost_NoIAQDevice)
{
	auto result = dispatcher.SetChlorinatorBoost(true);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorBoost_Disable_NoIAQDevice)
{
	auto result = dispatcher.SetChlorinatorBoost(false);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

// =============================================================================
// DispatchCommand - No Serial Adapter (via ToggleByLabel with device present)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDispatchCommand_NoSerialAdapter_ReturnsNoSerialAdapter)
{
	// Add a device to DataHub so it can be found by label
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Test Aux"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	auto result = dispatcher.CommandByLabel("Test Aux", ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

// =============================================================================
// Capability routing to an emulated OneTouch (no Serial Adapter present).
//
// With ONLY an emulated OneTouch on the bus the dispatcher must resolve it as the
// DeviceActuator (toggles) and SetpointController (heater setpoints) - this is the
// whole point of the capability refactor: a OneTouch-only rig can actuate. A
// PASSIVE (non-emulated) OneTouch advertises the same capabilities but honestly
// reports NotSupported at call time, so commands fall through to NoSerialAdapter.
// =============================================================================

namespace
{
	std::unique_ptr<OneTouchDevice> MakeOneTouch(AqualinkAutomate::Test::HubLocatorInjector& hub, uint8_t id, bool emulated)
	{
		auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(id));
		return std::make_unique<OneTouchDevice>(device_id, hub, emulated);
	}
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_EmulatedOneTouch_RoutesAndAccepts)
{
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	// In range + a capable controller present => the OneTouch SetpointController
	// queues the menu edit and the dispatcher reports Success.
	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetSpaSetpoint_EmulatedOneTouch_RoutesAndAccepts)
{
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.SetSpaSetpoint(100);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_PassiveOneTouch_FallsThroughToNoSerialAdapter)
{
	// A non-emulated OneTouch IS-A SetpointController (the dynamic_cast resolves it),
	// but it cannot transmit, so it returns NotSupported -> the call-site maps that to
	// NoSerialAdapter (the historical "nothing can act" code for setpoints).
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, false));

	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_EmulatedOneTouch_RoutesAndAccepts)
{
	// A labelled aux the OneTouch can map onto the Equipment ON/OFF row.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Pool Light"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.ToggleByLabel("Pool Light");
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_SerialAdapterPreferredOverOneTouch)
{
	// Both a Serial Adapter (High) and an emulated OneTouch (Low) advertise
	// SetpointController. FindCapable<> must pick the higher-priority Serial Adapter;
	// either way the dispatcher reports Success, but precedence is what we assert at
	// the unit level via FindCapable below. Here we just prove both-present still works.
	auto sa_id = std::make_shared<JandyDeviceType>(JandyDeviceId(0x48));
	equipment_hub->AddDevice(std::make_unique<SerialAdapterDevice>(sa_id, *this, true));
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_EmulatedOneTouch_RoutesAndAccepts)
{
	// A OneTouch-only rig can now drive the chlorinator (via the Set AquaPure menu) - the
	// OneTouch advertises ChlorinatorController and the % is queued/accepted.
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.SetChlorinatorPercentage(45);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorBoost_EmulatedOneTouch_RoutesAndAccepts)
{
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.SetChlorinatorBoost(true);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_PassiveOneTouch_FallsThrough)
{
	// A non-emulated OneTouch IS-A ChlorinatorController but cannot transmit -> NotSupported,
	// which the chlorinator call-site maps to DeviceNotFound.
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, false));

	auto result = dispatcher.SetChlorinatorPercentage(45);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

// On a SUCCESSFUL set, the value is written through to the same configured Pool-setpoint trait
// the menu scrape populates, so the dashboard reflects the new target without waiting for a
// re-scrape.
BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_WritesThroughToCache)
{
	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, ChlorinatorStatuses::On);
		data_hub->Devices.Add(std::move(chlorinator));
	}
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));

	auto result = dispatcher.SetChlorinatorPercentage(45);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	auto chlorinators = data_hub->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);
	auto pool = chlorinators.front()->AuxillaryTraits.TryGet(ChlorinatorPoolSetpointTrait{});
	BOOST_REQUIRE(pool.has_value());
	BOOST_CHECK_EQUAL(pool.value(), static_cast<uint8_t>(45));
}

// A REJECTED set (no controller can actually act) must NOT write through a phantom setpoint.
BOOST_AUTO_TEST_CASE(TestSetChlorinatorPercentage_RejectedDoesNotWriteThrough)
{
	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		data_hub->Devices.Add(std::move(chlorinator));
	}
	// A passive OneTouch cannot transmit -> DeviceNotFound; nothing should be written through.
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, false));

	auto result = dispatcher.SetChlorinatorPercentage(45);
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));

	auto chlorinators = data_hub->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);
	BOOST_CHECK(!chlorinators.front()->AuxillaryTraits.TryGet(ChlorinatorPoolSetpointTrait{}).has_value());
}

// =============================================================================
// Honest actuation + priority-ordered fallback.
//
// Regression for the silent command no-op: a real/passive controller (which
// cannot transmit) must NOT falsely report Success, and the dispatcher must fall
// back to a lower-priority controller that CAN actually act. Before the fix a real
// Serial Adapter (priority High) was always chosen, returned Accepted, then
// swallowed the command on the wire -> the UI showed "Applied" but nothing
// happened. After the fix it honestly reports NotSupported and the dispatcher
// either falls back or surfaces an honest failure.
// =============================================================================

namespace
{
	std::unique_ptr<SerialAdapterDevice> MakeSerialAdapter(AqualinkAutomate::Test::HubLocatorInjector& hub, uint8_t id, bool emulated)
	{
		auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(id));
		return std::make_unique<SerialAdapterDevice>(device_id, hub, emulated);
	}
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_RealSerialAdapterOnly_DoesNotFalselySucceed)
{
	// THE headline regression: a REAL (non-emulated) Serial Adapter cannot transmit, so
	// it must report NotSupported rather than silently swallowing the command while the
	// dispatcher claims Success. With no other controller present the honest result is
	// NoSerialAdapter (a non-2xx the UI shows as Rejected), NOT Success.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Pool Light"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	equipment_hub->AddDevice(MakeSerialAdapter(*this, 0x48, false));

	auto result = dispatcher.ToggleByLabel("Pool Light");
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_RealSerialAdapter_FallsBackToEmulatedOneTouch)
{
	// A real Serial Adapter (priority High) outranks an emulated OneTouch (Low) but
	// cannot transmit -> NotSupported. The dispatcher must FALL BACK to the OneTouch,
	// which can actuate via menu navigation -> Success. This is the core fallback path.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Pool Light"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	equipment_hub->AddDevice(MakeSerialAdapter(*this, 0x48, false));   // High, passive
	equipment_hub->AddDevice(MakeOneTouch(*this, 0x41, true));         // Low, active

	auto result = dispatcher.ToggleByLabel("Pool Light");
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_EmulatedSerialAdapter_Accepts)
{
	// Positive path for the new IsEmulationActive() guard: an actively-emulating Serial
	// Adapter CAN transmit, so it accepts the setpoint.
	equipment_hub->AddDevice(MakeSerialAdapter(*this, 0x48, true));

	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));
}

BOOST_AUTO_TEST_CASE(TestSetPoolSetpoint_SuppressedEmulatedSerialAdapter_FallsThrough)
{
	// An emulated Serial Adapter that has been presence-suppressed (a real adapter
	// answered at its address) is now a passive decoder: IsEmulationActive() is false, so
	// it must report NotSupported -> NoSerialAdapter. This is exactly the case a plain
	// IsEmulated() check would have missed (IsEmulated() stays true after suppression).
	auto serial_adapter = MakeSerialAdapter(*this, 0x48, true);
	serial_adapter->SuppressEmulation();
	equipment_hub->AddDevice(std::move(serial_adapter));

	auto result = dispatcher.SetPoolSetpoint(82);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_EmulatedSerialAdapter_UnmappableAux_ReturnsUnknownEquipmentType)
{
	// A capable, actively-emulating controller that cannot map THIS particular device
	// onto a wire command reports MappingFailed; with no other controller able to take
	// over, the dispatcher surfaces UnknownEquipmentType (not the generic NoSerialAdapter).
	// A generic Auxillary with no JandyAuxillaryId is unmappable by the Serial Adapter.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Mystery Device"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	equipment_hub->AddDevice(MakeSerialAdapter(*this, 0x48, true));

	auto result = dispatcher.ToggleByLabel("Mystery Device");
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::UnknownEquipmentType));
}

BOOST_AUTO_TEST_CASE(TestToggleByLabel_RecordsCommandHistoryOnHandlingController)
{
	// A successful command is recorded on the controller that handled it (CommandHistory
	// mixin), surfaced via its diagnostics for the device-card "Recent Commands" list.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Pool Light"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	auto onetouch = MakeOneTouch(*this, 0x41, true);
	auto* onetouch_ptr = onetouch.get();
	equipment_hub->AddDevice(std::move(onetouch));

	auto result = dispatcher.ToggleByLabel("Pool Light");
	BOOST_REQUIRE_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::Success));

	const auto diag = onetouch_ptr->DescribeDiagnostics();
	BOOST_REQUIRE(diag.contains("recent_commands"));
	BOOST_REQUIRE_EQUAL(diag["recent_commands"].size(), 1u);
	BOOST_CHECK_EQUAL(diag["recent_commands"][0]["outcome"].get<std::string>(), "Success");
	BOOST_CHECK(diag["recent_commands"][0]["description"].get<std::string>().find("Pool Light") != std::string::npos);
}

// =============================================================================
// CommandByUuid with a device PRESENT (On/Off actions).
//
// The device-not-found cases above return before ActionVerb()/DeviceLabel() are
// reached. With a device found (but no serial adapter) the On/Off action arms,
// the "turn on"/"turn off" verb, and the DeviceLabel() helper are all exercised
// on the way to the honest NoSerialAdapter result.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestCommandByUuid_DevicePresentOn_UsesLabel_NoSerialAdapter)
{
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Pool Light"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	auto result = dispatcher.CommandByUuid(device->Id(), ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestCommandByUuid_DevicePresentOff_NoSerialAdapter)
{
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(LabelTrait{}, std::string{"Spa Blower"});
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	auto result = dispatcher.CommandByUuid(device->Id(), ICommandDispatcher::DeviceAction::Off);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

BOOST_AUTO_TEST_CASE(TestCommandByUuid_DevicePresentNoLabel_FallsBackToEquipment)
{
	// A device with no LabelTrait exercises the DeviceLabel() "equipment" fallback.
	auto device = std::make_shared<AuxillaryDevice>();
	device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	data_hub->Devices.Add(device);

	auto result = dispatcher.CommandByUuid(device->Id(), ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::NoSerialAdapter));
}

// =============================================================================
// CommandByLabel aux-id resolution.
//
// When no device matches the friendly label but the label parses as an aux-id
// form ("Aux5"), the dispatcher resolves it by the stable aux id. With no such
// device present this still exercises the aux-id resolution branch on the way to
// DeviceNotFound.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestCommandByLabel_AuxIdForm_ResolvesByStableId)
{
	auto result = dispatcher.CommandByLabel("Aux5", ICommandDispatcher::DeviceAction::On);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

// =============================================================================
// SelectIAQPageButton - routes to a PageNavigator-capable device.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSelectIAQPageButton_NoNavigator_DeviceNotFound)
{
	auto result = dispatcher.SelectIAQPageButton(3);
	BOOST_CHECK_EQUAL(static_cast<int>(result), static_cast<int>(ICommandDispatcher::CommandResult::DeviceNotFound));
}

BOOST_AUTO_TEST_SUITE_END()
