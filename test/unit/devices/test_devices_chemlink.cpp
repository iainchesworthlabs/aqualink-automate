#include <chrono>
#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/chemlink_device.h"
#include "devices/capabilities/restartable.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/chemlink/chemlink_message_response.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// Jandy Chemlink (chemistry feeder, device ids 0x80-0x83) device-handler tests.
//
// ChemlinkDevice takes only a device id (no HubLocator); it is constructed
// standalone and kept alive on the stack, and its response slot connects to the
// per-message static signal on construction, so replayed frames from the
// MockReplayHarness drive it.  Each frame is addressed to the feeder's own bus
// id so the destination-id slot filter admits it.
//
// Chemlink_Response field offsets (absolute into the framed packet, payload at
// index 4): DataTag[4], RawValue = U16LE at [5..6].  The device's slot maps each
// DataTag to a stored value:
//   ORP       -> millivolts = raw * 10
//   pH        -> pH = raw / 10.0
//   pHFeeder  -> running = (raw == 0x00)
//   ORPFeeder -> running = (raw != 0x00)
//=============================================================================

namespace
{
	constexpr uint8_t CHEMLINK_DEVICE_ID = 0x80;
	const uint8_t CMD_CHEMLINK_RESPONSE = static_cast<uint8_t>(Messages::JandyMessageIds::Chemlink_Response);

	// Pins Now() to the epoch so PollAll() (real clock) always finds the deadline
	// past and fires the device's real WatchdogTimeoutOccurred().
	struct TestChemlinkDevice : public ChemlinkDevice
	{
		using ChemlinkDevice::ChemlinkDevice;
		std::chrono::steady_clock::time_point Now() const override
		{
			return std::chrono::steady_clock::time_point{};
		}
		// The base ctor arms the watchdog before this override is live, so re-kick once
		// the override is active to move the deadline back to the epoch.
		void RearmAtEpoch() { Kick(); }
	};
}

BOOST_AUTO_TEST_SUITE(ChemlinkDevice_TestSuite)

// -----------------------------------------------------------------------------
// Construction defaults.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Construction_DefaultsAreZeroAndFalse)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	BOOST_CHECK_EQUAL(device.ORPMillivolts().first, static_cast<uint16_t>(0));
	BOOST_CHECK_CLOSE(device.PHValue().first, 0.0, 0.0001);
	BOOST_CHECK_EQUAL(device.PHFeederRunning().first, false);
	BOOST_CHECK_EQUAL(device.ORPFeederRunning().first, false);
}

// -----------------------------------------------------------------------------
// ORP tag -> millivolts = raw * 10.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_OrpTag_DecodesMillivolts)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	// DataTag=ORP(0x02), RawValue=0x0046 (70) -> 700 millivolts.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		CHEMLINK_DEVICE_ID, CMD_CHEMLINK_RESPONSE,
		{ static_cast<uint8_t>(Messages::ChemlinkDataTag::ORP), 0x46, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_EQUAL(device.ORPMillivolts().first, static_cast<uint16_t>(700));
}

// -----------------------------------------------------------------------------
// pH tag -> pH = raw / 10.0.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_PhTag_DecodesPhValue)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	// DataTag=pH(0x03), RawValue=0x0048 (72) -> pH 7.2.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		CHEMLINK_DEVICE_ID, CMD_CHEMLINK_RESPONSE,
		{ static_cast<uint8_t>(Messages::ChemlinkDataTag::pH), 0x48, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_CLOSE(device.PHValue().first, 7.2, 0.0001);
}

// -----------------------------------------------------------------------------
// pHFeeder tag -> running when raw == 0x00.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_PhFeederTag_RunningWhenZero)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		CHEMLINK_DEVICE_ID, CMD_CHEMLINK_RESPONSE,
		{ static_cast<uint8_t>(Messages::ChemlinkDataTag::pHFeeder), 0x00, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_EQUAL(device.PHFeederRunning().first, true);
}

// -----------------------------------------------------------------------------
// ORPFeeder tag -> running when raw != 0x00.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_OrpFeederTag_RunningWhenNonZero)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		CHEMLINK_DEVICE_ID, CMD_CHEMLINK_RESPONSE,
		{ static_cast<uint8_t>(Messages::ChemlinkDataTag::ORPFeeder), 0x01, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_EQUAL(device.ORPFeederRunning().first, true);
}

// -----------------------------------------------------------------------------
// An unrecognised DataTag byte maps to Unknown and takes the default arm of the
// slot's switch (no stored value mutates).
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_UnknownTag_TakesDefaultArm)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	ChemlinkDevice device(device_id);

	// 0x55 is not a defined ChemlinkDataTag -> enum_cast falls back to Unknown.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		CHEMLINK_DEVICE_ID, CMD_CHEMLINK_RESPONSE, { 0x55, 0x10, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	// Nothing was updated by the default arm.
	BOOST_CHECK_EQUAL(device.ORPMillivolts().first, static_cast<uint16_t>(0));
	BOOST_CHECK_CLOSE(device.PHValue().first, 0.0, 0.0001);
	BOOST_CHECK_EQUAL(device.PHFeederRunning().first, false);
	BOOST_CHECK_EQUAL(device.ORPFeederRunning().first, false);
}

// -----------------------------------------------------------------------------
// Watchdog timeout path (WatchdogTimeoutOccurred).
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(WatchdogTimeout_DoesNotThrow)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(CHEMLINK_DEVICE_ID));
	TestChemlinkDevice device(device_id);
	device.RearmAtEpoch();

	BOOST_CHECK_NO_THROW(Devices::Capabilities::Restartable::PollAll());
}

BOOST_AUTO_TEST_SUITE_END()
