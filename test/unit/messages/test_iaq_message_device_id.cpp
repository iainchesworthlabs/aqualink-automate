#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/messages/jandy_message_constants.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/iaq/iaq_message_device_id.h"

using namespace AqualinkAutomate::Messages;

BOOST_AUTO_TEST_SUITE(IAQMessage_DeviceIdTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction)
{
	IAQMessage_DeviceId message;
	BOOST_CHECK(message.Id() == JandyMessageIds::IAQ_DeviceId);
	BOOST_CHECK(message.DeviceId().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_DecodesAsciiSerial)
{
	// Captured boot-init frame (test/fixtures/iaq_boot_sequence.cap): the master sends the
	// iAqualink2 (0xa3) its serial via cmd 0x51 as a NUL-terminated ASCII string.
	//   DLE STX a3 51  "1BA62825B7C69A4C" 00  <checksum> DLE ETX
	const std::vector<uint8_t> frame =
	{
		HEADER_BYTE_DLE, HEADER_BYTE_STX, 0xa3, 0x51,
		'1','B','A','6','2','8','2','5','B','7','C','6','9','A','4','C', 0x00,
		0xa4,
		HEADER_BYTE_DLE, HEADER_BYTE_ETX
	};

	IAQMessage_DeviceId message;
	BOOST_REQUIRE(message.DeserializeContents(frame));
	BOOST_CHECK_EQUAL(message.DeviceId(), std::string("1BA62825B7C69A4C"));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ReturnsFalse)
{
	const std::vector<uint8_t> frame = { HEADER_BYTE_DLE, HEADER_BYTE_STX, 0xa3, 0x51, HEADER_BYTE_DLE, HEADER_BYTE_ETX };
	IAQMessage_DeviceId message;
	BOOST_CHECK_EQUAL(false, message.DeserializeContents(frame));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	// Receive-only message: the master originates it, so serialisation is never done.
	IAQMessage_DeviceId message;
	std::vector<uint8_t> out;
	BOOST_CHECK_EQUAL(false, message.SerializeContents(out));
}

BOOST_AUTO_TEST_CASE(TestToString_ContainsDecodedDeviceId)
{
	// ToString() prints the base packet summary plus the decoded id in single quotes.
	const std::vector<uint8_t> frame =
	{
		HEADER_BYTE_DLE, HEADER_BYTE_STX, 0xa3, 0x51,
		'1','B','A','6','2','8','2','5','B','7','C','6','9','A','4','C', 0x00,
		0xa4,
		HEADER_BYTE_DLE, HEADER_BYTE_ETX
	};

	IAQMessage_DeviceId message;
	BOOST_REQUIRE(message.DeserializeContents(frame));

	const auto str = message.ToString();
	BOOST_CHECK(str.find("DeviceId: '1BA62825B7C69A4C'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestToString_EmptyDeviceIdByDefault)
{
	// A default-constructed (never-deserialised) message prints an empty id.
	IAQMessage_DeviceId message;
	const auto str = message.ToString();
	BOOST_CHECK(str.find("DeviceId: ''") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
