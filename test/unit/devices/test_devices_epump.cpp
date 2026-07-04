#include <chrono>
#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/epump_device.h"
#include "devices/capabilities/restartable.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// Jandy ePump (device ids 0x78-0x7B) device-handler tests.
//
// EPumpDevice takes only a device id (no HubLocator), so it is constructed
// standalone and kept alive on the stack; its message slots connect to the
// per-message static signal on construction, so the MockReplayHarness's replayed
// frames drive them without the harness having to own the device.  Each frame is
// addressed to the pump's own bus id so the destination-id slot filter admits it.
//
// The RPM/Watts fields sit at absolute offsets into the framed packet:
//   RPM   = U16LE at payload bytes [2..3]  (frame indices 6..7), divided by 4.
//   Watts = U16LE at payload bytes [3..4]  (frame indices 7..8).
//=============================================================================

namespace
{
	constexpr uint8_t EPUMP_DEVICE_ID = 0x78;

	// Pins Now() to the steady-clock epoch; after RearmAtEpoch() re-kicks the
	// watchdog (deadline -> epoch), Restartable::PollAll() reads the REAL clock and
	// therefore always finds the deadline past, firing the device's real handler.
	struct TestEPumpDevice : public EPumpDevice
	{
		using EPumpDevice::EPumpDevice;
		std::chrono::steady_clock::time_point Now() const override
		{
			return std::chrono::steady_clock::time_point{};
		}
		// The base ctor arms the watchdog before this override is live (virtual calls in
		// a base ctor resolve to the base), so m_LastKick lands on the REAL clock. Re-kick
		// now that the override is active to move the deadline to the epoch.
		void RearmAtEpoch() { Kick(); }
	};
}

BOOST_AUTO_TEST_SUITE(EPumpDevice_TestSuite)

// -----------------------------------------------------------------------------
// Construction defaults.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Construction_DefaultsAreZero)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	EPumpDevice device(device_id);

	BOOST_CHECK_EQUAL(device.ReportedRPM().first, static_cast<uint16_t>(0));
	BOOST_CHECK_EQUAL(device.ReportedWatts().first, static_cast<uint16_t>(0));
}

// -----------------------------------------------------------------------------
// EPUMP_RPM frame decodes into ReportedRPM (raw quarter-RPM field / 4).
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_RpmFrame_DecodesReportedRpm)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	EPumpDevice device(device_id);

	// Payload bytes: [sub, sub, rpm_low, rpm_high]. RPM field at frame index 6/7.
	// 0x2000 LE = 8192 quarter-RPM -> 2048 RPM.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		EPUMP_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::EPUMP_RPM),
		{ 0x00, 0x00, 0x00, 0x20 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_EQUAL(device.ReportedRPM().first, static_cast<uint16_t>(2048));
}

// -----------------------------------------------------------------------------
// EPUMP_Watts frame decodes into ReportedWatts.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_WattsFrame_DecodesReportedWatts)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	EPumpDevice device(device_id);

	// Watts field is U16LE at frame indices 7/8 -> payload bytes [3..4].
	// 0x0190 LE = 400 watts.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		EPUMP_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::EPUMP_Watts),
		{ 0x00, 0x00, 0x00, 0x90, 0x01 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_CHECK_EQUAL(device.ReportedWatts().first, static_cast<uint16_t>(400));
}

// -----------------------------------------------------------------------------
// EPUMP_Status frame is handled (kicks the watchdog); it carries no field the
// device stores, so we assert it is consumed without a checksum failure.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_StatusFrame_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	EPumpDevice device(device_id);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		EPUMP_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::EPUMP_Status),
		{ 0x42 });
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

// -----------------------------------------------------------------------------
// A frame addressed to a DIFFERENT pump id must not mutate this device.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_RpmForDifferentId_IsIgnored)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	EPumpDevice device(device_id);

	// Addressed to 0x79, not this device's 0x78.
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		0x79, static_cast<uint8_t>(Messages::JandyMessageIds::EPUMP_RPM),
		{ 0x00, 0x00, 0x00, 0x20 });
	harness.Replay(frame);

	BOOST_CHECK_EQUAL(device.ReportedRPM().first, static_cast<uint16_t>(0));
}

// -----------------------------------------------------------------------------
// Watchdog timeout path (WatchdogTimeoutOccurred).
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(WatchdogTimeout_DoesNotThrow)
{
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(EPUMP_DEVICE_ID));
	TestEPumpDevice device(device_id);
	device.RearmAtEpoch();

	BOOST_CHECK_NO_THROW(Devices::Capabilities::Restartable::PollAll());
}

BOOST_AUTO_TEST_SUITE_END()
