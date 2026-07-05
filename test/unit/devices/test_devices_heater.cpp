#include <chrono>
#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/heater_device.h"
#include "devices/capabilities/restartable.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/heater/heater_message_request.h"
#include "jandy/messages/heater/heater_message_status.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// Jandy heater device-handler tests.
//
// HeaterDevice takes only a device id (no HubLocator); it is constructed
// standalone and kept alive on the stack, and its message slots connect to the
// per-message static signal on construction, so replayed frames from the
// MockReplayHarness drive them.  Frames are addressed to the heater's own bus id
// so the destination-id slot filter admits them.
//
// Field offsets (absolute into the framed packet, payload starts at index 4):
//   Heater_Request: mode[4], pool setpoint[5], spa setpoint[6], water temp[7].
//   Heater_Status : state[4], (reserved[5]), error[6].
//=============================================================================

namespace
{
	constexpr uint8_t HEATER_DEVICE_ID = 0x68;   // A JXi heater bus id.

	// Pins Now() to the epoch so PollAll() (real clock) always finds the deadline
	// past and fires the device's real WatchdogTimeoutOccurred().
	struct TestHeaterDevice : public HeaterDevice
	{
		using HeaterDevice::HeaterDevice;
		std::chrono::steady_clock::time_point Now() const override
		{
			return std::chrono::steady_clock::time_point{};
		}
		// The base ctor arms the watchdog before this override is live, so re-kick once
		// the override is active to move the deadline back to the epoch.
		void RearmAtEpoch() { Kick(); }
	};
}

BOOST_AUTO_TEST_SUITE(HeaterDevice_TestSuite)

// -----------------------------------------------------------------------------
// Construction defaults.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Construction_DefaultsAreUnknownAndZero)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(HEATER_DEVICE_ID));
	HeaterDevice device(device_id);

	BOOST_CHECK(device.OperatingMode().first == Messages::HeaterOperatingModes::Unknown);
	BOOST_CHECK_EQUAL(device.PoolSetpoint().first, static_cast<uint8_t>(0));
	BOOST_CHECK_EQUAL(device.SpaSetpoint().first, static_cast<uint8_t>(0));
	BOOST_CHECK_EQUAL(device.WaterTemperature().first, static_cast<uint8_t>(0));
	BOOST_CHECK(device.HeaterState().first == Messages::HeaterStates::Unknown);
	BOOST_CHECK(device.ErrorCode().first == Messages::HeaterErrors::Unknown);
}

// -----------------------------------------------------------------------------
// Heater_Request frame decodes mode + setpoints + water temperature.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_RequestFrame_DecodesModeSetpointsAndTemp)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(HEATER_DEVICE_ID));
	HeaterDevice device(device_id);

	// mode=HeatingPool(0x19), pool=90, spa=104, water=85.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		HEATER_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Heater_Request),
		{ 0x19, 90, 104, 85 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK(device.OperatingMode().first == Messages::HeaterOperatingModes::HeatingPool);
	BOOST_CHECK_EQUAL(device.PoolSetpoint().first, static_cast<uint8_t>(90));
	BOOST_CHECK_EQUAL(device.SpaSetpoint().first, static_cast<uint8_t>(104));
	BOOST_CHECK_EQUAL(device.WaterTemperature().first, static_cast<uint8_t>(85));
}

// -----------------------------------------------------------------------------
// Heater_Status frame decodes heater state + error code.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_StatusFrame_DecodesStateAndError)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(HEATER_DEVICE_ID));
	HeaterDevice device(device_id);

	// state=Heating(0x08) at [4], reserved 0x00 at [5], error=HighLimit(0x10) at [6].
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		HEATER_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Heater_Status),
		{ 0x08, 0x00, 0x10 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK(device.HeaterState().first == Messages::HeaterStates::Heating);
	BOOST_CHECK(device.ErrorCode().first == Messages::HeaterErrors::HighLimit);
}

// -----------------------------------------------------------------------------
// A frame addressed to a different heater id must not mutate this device.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_StatusForDifferentId_IsIgnored)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(HEATER_DEVICE_ID));
	HeaterDevice device(device_id);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		0x69, static_cast<uint8_t>(Messages::JandyMessageIds::Heater_Status),
		{ 0x08, 0x00, 0x10 });
	harness.Replay(frame);

	BOOST_CHECK(device.HeaterState().first == Messages::HeaterStates::Unknown);
}

// -----------------------------------------------------------------------------
// Watchdog timeout path (WatchdogTimeoutOccurred).
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(WatchdogTimeout_DoesNotThrow)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(HEATER_DEVICE_ID));
	TestHeaterDevice device(device_id);
	device.RearmAtEpoch();

	BOOST_CHECK_NO_THROW(Devices::Capabilities::Restartable::PollAll());
}

BOOST_AUTO_TEST_SUITE_END()
