#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/keypad_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

namespace
{
	struct KeypadDeviceFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		KeypadDeviceFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x08)))
		{
		}

		std::shared_ptr<JandyDeviceType> device_type;
	};
}

BOOST_FIXTURE_TEST_SUITE(KeypadDevice_TestSuite, KeypadDeviceFixture)

// =============================================================================
// Construction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_Emulated)
{
	BOOST_CHECK_NO_THROW(KeypadDevice device(device_type, *this, true));
}

BOOST_AUTO_TEST_CASE(TestConstruction_NonEmulated)
{
	BOOST_CHECK_NO_THROW(KeypadDevice device(device_type, *this, false));
}

// =============================================================================
// Destruction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDestruction_CleanAfterConstruction)
{
	{
		KeypadDevice device(device_type, *this, true);
	}
	BOOST_CHECK(true);
}

// =============================================================================
// Multiple device IDs in the Keypad range
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_DifferentDeviceId)
{
	auto device_type_09 = std::make_shared<JandyDeviceType>(JandyDeviceId(0x09));
	BOOST_CHECK_NO_THROW(KeypadDevice device(device_type_09, *this, true));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Message-processor slot coverage.
//
// The RS keypad's message slots are wired to the per-message static signal, so
// a keypad constructed against the harness's HubLocator takes part in the full
// decode path.  Each synthetic frame is addressed to the keypad's own bus id so
// the RegisterSlot_FilterByDeviceId destination filter admits it, then Replay()
// drives the frame through the real generator + static-signal dispatch into the
// matching Slot_Keypad_* handler.  A NON-emulated keypad is used so the Ack slot
// (registered only for the non-emulated role) participates too.
//=============================================================================

namespace
{
	constexpr uint8_t KEYPAD_DEVICE_ID = 0x08;
}

BOOST_AUTO_TEST_SUITE(Keypad_MessageProcessors_TestSuite)

BOOST_AUTO_TEST_CASE(Replay_KeypadProbe_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(KEYPAD_DEVICE_ID));
	KeypadDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		KEYPAD_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Probe), {});
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

BOOST_AUTO_TEST_CASE(Replay_KeypadAck_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(KEYPAD_DEVICE_ID));
	// Ack slot is only registered for the NON-emulated keypad role.
	KeypadDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		KEYPAD_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Ack), {});
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

BOOST_AUTO_TEST_CASE(Replay_KeypadStatus_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(KEYPAD_DEVICE_ID));
	KeypadDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		KEYPAD_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Status),
		{ 0x00, 0x00, 0x00, 0x00, 0x00 });
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

BOOST_AUTO_TEST_CASE(Replay_KeypadMessage_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(KEYPAD_DEVICE_ID));
	KeypadDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		KEYPAD_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::Message),
		{ 'H', 'E', 'L', 'L', 'O' });
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

BOOST_AUTO_TEST_CASE(Replay_KeypadMessageLong_IsHandled)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(KEYPAD_DEVICE_ID));
	KeypadDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(
		KEYPAD_DEVICE_ID, static_cast<uint8_t>(Messages::JandyMessageIds::MessageLong),
		{ 'L', 'O', 'N', 'G' });
	BOOST_CHECK_NO_THROW(harness.Replay(frame));

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
