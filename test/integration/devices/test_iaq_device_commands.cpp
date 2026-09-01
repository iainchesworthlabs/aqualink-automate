#include <cstdint>
#include <memory>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/iaq/iaq_message_poll.h"
#include "jandy/messages/jandy_message_ack.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Messages;
using namespace AqualinkAutomate::Interfaces;

// =============================================================================
// Helper: build a valid Poll packet with correct checksum for device 0x33
// Packet: [DLE, STX, dest, msg_type, checksum, DLE, ETX]
// =============================================================================

namespace
{
	std::vector<uint8_t> MakeValidPollPacket(uint8_t dest)
	{
		// Poll message (0x30) with no data payload
		std::vector<uint8_t> pkt = { 0x10, 0x02, dest, 0x30 };
		// Checksum = sum of all bytes before footer
		uint32_t sum = 0;
		for (auto b : pkt) sum += b;
		pkt.push_back(static_cast<uint8_t>(sum & 0xFF));
		pkt.push_back(0x10);
		pkt.push_back(0x03);
		return pkt;
	}

	struct IAQDeviceCommandFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		static constexpr uint8_t DEVICE_ID = 0x33;

		IAQDeviceCommandFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(DEVICE_ID)))
			, device(device_type, *this, true) // emulated=true so ACKs are sent
		{
			// Connect to the Ack send signal to capture outgoing ACK commands
			ack_connection = IMessageSignalSend<JandyMessage_Ack>::GetPublisher()->connect(
				[this](std::reference_wrapper<const JandyMessage_Ack> ack_ref)
				{
					captured_ack_commands.push_back(ack_ref.get().Command());
				}
			);
		}

		~IAQDeviceCommandFixture() override
		{
			ack_connection.disconnect();
		}

		void FirePoll()
		{
			IAQMessage_Poll poll;
			auto pkt = MakeValidPollPacket(DEVICE_ID);
			[[maybe_unused]] auto result = poll.Deserialize(std::span<const uint8_t>(pkt));
			poll.Signal_MessageWasReceived();
		}

		std::shared_ptr<JandyDeviceType> device_type;
		IAQDevice device;
		std::vector<uint8_t> captured_ack_commands;
		boost::signals2::connection ack_connection;
	};
}

BOOST_FIXTURE_TEST_SUITE(IAQDeviceCommands_IntegrationTestSuite, IAQDeviceCommandFixture)

// =============================================================================
// Poll with no commands -> empty ACK (command = 0x00)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPoll_NoCommands_EmptyAck)
{
	FirePoll();

	BOOST_REQUIRE(!captured_ack_commands.empty());
	BOOST_CHECK_EQUAL(captured_ack_commands.back(), static_cast<uint8_t>(0x00));
}

// =============================================================================
// QueueCommand then poll -> command dispatched, then empty ACK
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueCommand_ThenPoll_DispatchesCommand)
{
	device.QueueCommand(0x19);
	captured_ack_commands.clear();

	FirePoll();

	BOOST_REQUIRE(!captured_ack_commands.empty());
	BOOST_CHECK_EQUAL(captured_ack_commands.back(), static_cast<uint8_t>(0x19));

	// Next poll should send empty ACK
	captured_ack_commands.clear();
	FirePoll();
	BOOST_REQUIRE(!captured_ack_commands.empty());
	BOOST_CHECK_EQUAL(captured_ack_commands.back(), static_cast<uint8_t>(0x00));
}

// =============================================================================
// The chlorinator sequence is no longer a blind queued burst.
//
// It used to be four commands pushed at queue time, assuming the AquaPure page
// always opened from a FIXED page-button index. The master lays every page out
// from the installed equipment, so that index is not portable -- and 0x02 is the
// panel's single "Menu / Back" key, whose meaning depends on the screen it is
// pressed from, so the burst could not even rely on reaching the menu.
//
// It is now produced a command at a time by the page-gated AquaPureWriter, which
// verifies each navigation hop and resolves buttons by their on-screen label.
// Covered in test/unit/devices/test_devices_iaq_aquapure_writer.cpp (the state
// machine) and integration/flows/test_flow_command_to_wire.cpp (to the wire).
// =============================================================================

BOOST_AUTO_TEST_SUITE_END()
