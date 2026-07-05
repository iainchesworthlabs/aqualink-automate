#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"

#include "devices/capabilities/restartable.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/lights_device.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/light/light_message_status.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

//=============================================================================
// Jandy colored-light controller (device ids 0xF0-0xF4) replay tests.
//
// These exercise the FULL Jandy decode stack via the MockReplayHarness: a
// synthetic Light_Status frame is fed into the protocol task, framed/checksum-
// validated by the real generator, dispatched via the per-type static signal,
// and consumed by the LightsDevice's slot.  We then assert the decoded state on
// the device object.
//=============================================================================

namespace
{
	constexpr uint8_t LIGHT_DEVICE_ID = 0xF0;          // First Jandy light controller.
	const uint8_t CMD_LIGHT_STATUS = static_cast<uint8_t>(Messages::JandyMessageIds::Light_Status);
}

BOOST_AUTO_TEST_SUITE(TestSuite_Devices_Lights)

//-----------------------------------------------------------------------------
// A light "on" status frame decodes into the device's on/off + mode state.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_LightStatusOn_DecodesIntoDevice)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto& device = harness.AddDevice<LightsDevice>(device_id);

	// State byte 0x01 (on), mode byte 0x07.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		LIGHT_DEVICE_ID, CMD_LIGHT_STATUS, { 0x01, 0x07 });

	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	BOOST_CHECK(device.LightState().first == Messages::LightStates::On);
	BOOST_CHECK_EQUAL(device.IsOn(), true);
	BOOST_CHECK_EQUAL(device.LightMode().first, 0x07);

	// Hub-level decoded state: decoding the status frame creates a light in the
	// DataHub and stamps its on/off status + colour/mode onto it.
	auto lights = harness.DataHub()->Lights();
	BOOST_REQUIRE_EQUAL(lights.size(), 1u);

	auto status_trait = lights.front()->AuxillaryTraits.TryGet(AuxillaryStatusTrait{});
	BOOST_REQUIRE(status_trait.has_value());
	BOOST_CHECK(status_trait.value() == Kernel::AuxillaryStatuses::On);

	auto colour_trait = lights.front()->AuxillaryTraits.TryGet(ColourTrait{});
	BOOST_REQUIRE(colour_trait.has_value());
	BOOST_CHECK_EQUAL(colour_trait.value(), static_cast<uint8_t>(0x07));

	// A SECOND status frame must NOT create a duplicate light: EnsureLightDeviceExists
	// takes the "already exists" early-return path and the single light is updated.
	auto frame2 = Test::MessageBuilder::CreateValidChecksummedMessage(
		LIGHT_DEVICE_ID, CMD_LIGHT_STATUS, { 0x01, 0x03 });
	harness.Replay(frame2);

	auto lights_after = harness.DataHub()->Lights();
	BOOST_REQUIRE_EQUAL(lights_after.size(), 1u);
	auto colour_after = lights_after.front()->AuxillaryTraits.TryGet(ColourTrait{});
	BOOST_REQUIRE(colour_after.has_value());
	BOOST_CHECK_EQUAL(colour_after.value(), static_cast<uint8_t>(0x03));
}

//-----------------------------------------------------------------------------
// A light status carrying an out-of-range state byte (0xFF) decodes to the
// Unknown state and maps, via ConvertToAuxillaryStatus's default arm, to the
// Unknown auxillary status.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_LightStatusUnknownState_MapsToUnknownAuxStatus)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto& device = harness.AddDevice<LightsDevice>(device_id);

	// State byte 0xFF is neither On (0x01) nor Off (0x00).
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		LIGHT_DEVICE_ID, CMD_LIGHT_STATUS, { 0xFF, 0x00 });
	harness.Replay(frame);

	BOOST_CHECK(device.LightState().first == Messages::LightStates::Unknown);

	auto lights = harness.DataHub()->Lights();
	BOOST_REQUIRE_EQUAL(lights.size(), 1u);

	auto status_trait = lights.front()->AuxillaryTraits.TryGet(AuxillaryStatusTrait{});
	BOOST_REQUIRE(status_trait.has_value());
	BOOST_CHECK(status_trait.value() == Kernel::AuxillaryStatuses::Unknown);
}

//-----------------------------------------------------------------------------
// A light "off" status frame decodes into the device's state.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_LightStatusOff_DecodesIntoDevice)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto& device = harness.AddDevice<LightsDevice>(device_id);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		LIGHT_DEVICE_ID, CMD_LIGHT_STATUS, { 0x00, 0x00 });

	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	BOOST_CHECK(device.LightState().first == Messages::LightStates::Off);
	BOOST_CHECK_EQUAL(device.IsOn(), false);
	BOOST_CHECK_EQUAL(device.LightMode().first, 0x00);
}

//-----------------------------------------------------------------------------
// The per-device slot filters by destination id: a frame addressed to a
// DIFFERENT light controller (0xF1) must NOT mutate this device (0xF0).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_LightStatusForDifferentId_IsIgnored)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto& device = harness.AddDevice<LightsDevice>(device_id);

	// Addressed to 0xF1, not the device's 0xF0.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		0xF1, CMD_LIGHT_STATUS, { 0x01, 0x05 });

	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	// Device state stays at its constructed default (no update for 0xF1).
	BOOST_CHECK(device.LightState().first == Messages::LightStates::Unknown);
	BOOST_CHECK_EQUAL(device.IsOn(), false);
}

//-----------------------------------------------------------------------------
// Watchdog timeout path.
//
// WatchdogTimeoutOccurred() is a private override, so it is driven through the
// public Restartable::PollAll() sweep.  The test subclass pins Now() to the
// steady-clock epoch and RearmAtEpoch() re-kicks the watchdog once that override
// is live (m_LastKick -> epoch), while PollAll() reads the REAL clock, so the
// deadline is always past and the sweep fires the device's real timeout handler.
//-----------------------------------------------------------------------------
namespace
{
	struct TestLightsDevice : public LightsDevice
	{
		using LightsDevice::LightsDevice;
		std::chrono::steady_clock::time_point Now() const override
		{
			return std::chrono::steady_clock::time_point{};
		}
		// The base ctor arms the watchdog before this override is live, so re-kick once
		// the override is active to move the deadline back to the epoch.
		void RearmAtEpoch() { Kick(); }
	};
}

// Timeout with a light already present in the DataHub: the found light has its
// status trait reset to Unknown (the "lost comms" reflection).
BOOST_AUTO_TEST_CASE(WatchdogTimeout_WithKnownLight_ResetsStatusToUnknown)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto device = std::make_shared<TestLightsDevice>(device_id, harness.HubLocatorRef());

	// A status frame both connects the device's decode and creates the light so
	// the timeout handler's "found by label" branch is exercised.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		LIGHT_DEVICE_ID, CMD_LIGHT_STATUS, { 0x01, 0x07 });
	harness.Replay(frame);

	auto lights = harness.DataHub()->Lights();
	BOOST_REQUIRE_EQUAL(lights.size(), 1u);
	{
		auto status_trait = lights.front()->AuxillaryTraits.TryGet(AuxillaryStatusTrait{});
		BOOST_REQUIRE(status_trait.has_value());
		BOOST_CHECK(status_trait.value() == Kernel::AuxillaryStatuses::On);
	}

	// Sweep the watchdog; the device's timeout fires and reflects lost comms.
	device->RearmAtEpoch();
	Devices::Capabilities::Restartable::PollAll();

	auto status_trait = lights.front()->AuxillaryTraits.TryGet(AuxillaryStatusTrait{});
	BOOST_REQUIRE(status_trait.has_value());
	BOOST_CHECK(status_trait.value() == Kernel::AuxillaryStatuses::Unknown);
}

// Timeout with NO light present: the handler must not throw when FindByLabel is
// empty (the early-return-after-empty branch).
BOOST_AUTO_TEST_CASE(WatchdogTimeout_WithNoLight_DoesNotThrow)
{
	Test::HubLocatorInjector hub_locator;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(LIGHT_DEVICE_ID));
	auto device = std::make_shared<TestLightsDevice>(device_id, hub_locator);
	device->RearmAtEpoch();

	BOOST_CHECK_NO_THROW(Devices::Capabilities::Restartable::PollAll());
}

BOOST_AUTO_TEST_SUITE_END()
