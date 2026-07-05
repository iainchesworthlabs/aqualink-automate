#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/formatters/jandy_device_formatters.h"
#include "jandy/formatters/jandy_message_formatters.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/jandy_message_message_long.h"

using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Messages;

BOOST_AUTO_TEST_SUITE(JandyMessage_MessageLongTestSuite)

BOOST_AUTO_TEST_CASE(TestJandyMessage_MessageLongConstruction)
{
    JandyMessage_MessageLong message;
    BOOST_CHECK_EQUAL(message.Destination(), JandyDeviceId(0x00));
    BOOST_CHECK_EQUAL(message.Id(), JandyMessageIds::MessageLong);
    BOOST_CHECK_EQUAL(message.RawId(), 0x0);
    BOOST_CHECK_EQUAL(message.MessageLength(), 0);
    BOOST_CHECK_EQUAL(message.ChecksumValue(), 0);

    BOOST_CHECK_EQUAL(message.LineId(), 0);
    BOOST_CHECK_EQUAL(message.Line(), "");
}

BOOST_AUTO_TEST_CASE(TestSerializationDeserialization)
{
    const std::string TEST_LINE {"TEST DATA HERE!!"};  // 16 chars = DISPLAY_LINE_LENGTH
    const uint8_t TEST_LINE_ID{ 5 };

    JandyMessage_MessageLong message1(TEST_LINE_ID, TEST_LINE);
    JandyMessage_MessageLong message2;

    std::vector<uint8_t> serializedMessage;
    BOOST_REQUIRE(message1.Serialize(serializedMessage));
    BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(serializedMessage))));

    BOOST_CHECK_EQUAL(message1.Destination(), message2.Destination());
    BOOST_CHECK_EQUAL(message1.Id(), message2.Id());
    BOOST_CHECK_NE(message1.RawId(), message2.RawId()); // Deserialisation captures the message "raw" id...
    BOOST_CHECK_EQUAL(0x04, message2.RawId());
    BOOST_CHECK_NE(message1.MessageLength(), message2.MessageLength());  // Deserialisation captures the message length...
    BOOST_CHECK_EQUAL(24, message2.MessageLength());
    BOOST_CHECK_NE(message1.ChecksumValue(), message2.ChecksumValue());  // Deserialisation captures the message checksum value...
    BOOST_CHECK_EQUAL(0x1B, message2.ChecksumValue());

    BOOST_CHECK_EQUAL(TEST_LINE, message2.Line());
    BOOST_CHECK_EQUAL(TEST_LINE_ID, message2.LineId());
}

BOOST_AUTO_TEST_CASE(TestToString)
{
    JandyMessage_MessageLong message;

    const std::string expected = "Packet: Destination: AqualinkMaster (0x00), Message Type: MessageLong (0x04) || Payload: LineId: 0, Line: ''";

    BOOST_CHECK_EQUAL(message.ToString(), expected);
}

BOOST_AUTO_TEST_CASE(TestDoubleDeserialize_DoesNotAppend)
{
    // Regression test: deserializing twice must replace, not append.
    const std::string LINE_A{"FIRST LINE DATA!"};
    const std::string LINE_B{"SECOND LINE !!! "};

    JandyMessage_MessageLong src_a(1, LINE_A);
    JandyMessage_MessageLong src_b(2, LINE_B);

    std::vector<uint8_t> bytes_a, bytes_b;
    BOOST_REQUIRE(src_a.Serialize(bytes_a));
    BOOST_REQUIRE(src_b.Serialize(bytes_b));

    JandyMessage_MessageLong target;
    BOOST_REQUIRE(target.Deserialize(std::as_bytes(std::span<uint8_t>(bytes_a))));
    BOOST_CHECK_EQUAL(target.Line(), LINE_A);

    BOOST_REQUIRE(target.Deserialize(std::as_bytes(std::span<uint8_t>(bytes_b))));
    BOOST_CHECK_EQUAL(target.Line(), LINE_B);
}

BOOST_AUTO_TEST_CASE(TestDeserializeContentsTooShortForLineId)
{
    // A packet whose span does not even reach the LineId byte (absolute index 4) must be
    // rejected by the RequireIndex guard before any field is read. DeserializeContents is
    // the code under test directly (bypassing framing) so the too-short guard is reachable.
    JandyMessage_MessageLong message;

    const std::vector<uint8_t> too_short{ 0x00, 0x04, 0x00, 0x00 };  // size 4 -> index 4 out of range
    BOOST_CHECK(!message.DeserializeContents(std::span<const uint8_t>(too_short)));
}

BOOST_AUTO_TEST_CASE(TestDeserializeContentsTooShortForLineText)
{
    // Long enough for the LineId byte but with no room for even one LineText character above
    // the 3-byte footer (size <= Index_LineText + PACKET_FOOTER_LENGTH == 8) -> rejected.
    JandyMessage_MessageLong message;

    const std::vector<uint8_t> no_text(8, 0x00);  // index 4 present, but no LineText payload
    BOOST_CHECK(!message.DeserializeContents(std::span<const uint8_t>(no_text)));
}

BOOST_AUTO_TEST_CASE(TestDeserializeContentsClampsOverlongLineToDisplayLength)
{
    // A payload carrying more printable characters than a display line holds must be clamped
    // to DISPLAY_LINE_LENGTH (16). Build: 4 header bytes, LineId, 20 'A' text bytes, 3 footer.
    JandyMessage_MessageLong message;

    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), { 0x00, 0x04, 0x00, 0x07 });  // header (dest, id, ..)
    bytes.insert(bytes.end(), 20, static_cast<uint8_t>('A'));  // 20 printable LineText chars
    bytes.insert(bytes.end(), { 0x00, 0x00, 0x00 });         // 3-byte footer

    BOOST_REQUIRE(message.DeserializeContents(std::span<const uint8_t>(bytes)));
    BOOST_CHECK_EQUAL(message.Line().size(), 16U);        // clamped to DISPLAY_LINE_LENGTH
    BOOST_CHECK_EQUAL(message.Line(), std::string(16, 'A'));
}

BOOST_AUTO_TEST_SUITE_END()
