#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <magic_enum/magic_enum.hpp>

#include "jandy/devices/jandy_device_id.h"
#include "jandy/formatters/jandy_device_formatters.h"
#include "jandy/formatters/jandy_message_formatters.h"
#include "jandy/messages/jandy_message_constants.h"
#include "jandy/messages/jandy_message_ids.h"
#include "jandy/messages/aquarite/aquarite_message_ppm.h"
#include "jandy/utility/jandy_checksum.h"

#include "support/unit_test_ostream_support.h"

using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Messages;

BOOST_AUTO_TEST_SUITE(AquariteMessage_PPMTestSuite)

BOOST_AUTO_TEST_CASE(TestAquariteMessage_PPMConstruction)
{
    AquariteMessage_PPM message;

    BOOST_CHECK_EQUAL(message.Destination(), JandyDeviceId(0x00));
    BOOST_CHECK_EQUAL(message.Id(), JandyMessageIds::AQUARITE_PPM);
    BOOST_CHECK_EQUAL(message.RawId(), 0x0);
    BOOST_CHECK_EQUAL(message.MessageLength(), 0);
    BOOST_CHECK_EQUAL(message.ChecksumValue(), 0);

    BOOST_CHECK_EQUAL(message.SaltConcentrationPPM(), 0);
    BOOST_CHECK_EQUAL(message.Status(), AquariteStatuses::Unknown);

    const std::vector<AquariteStatuses> expected_flags{ AquariteStatuses::Unknown };
    BOOST_TEST(message.StatusFlags() == expected_flags, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(TestSerializationDeserialization)
{
    AquariteMessage_PPM message1;
    AquariteMessage_PPM message2;

    std::vector<uint8_t> serializedMessage;
    BOOST_REQUIRE(message1.Serialize(serializedMessage));
    BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(serializedMessage))));

    BOOST_CHECK_EQUAL(message1.Destination(), message2.Destination());
    BOOST_CHECK_EQUAL(message1.Id(), message2.Id());
    BOOST_CHECK_NE(message1.RawId(), message2.RawId()); // Deserialisation captures the message "raw" id...
    BOOST_CHECK_EQUAL(0x16, message2.RawId());
    BOOST_CHECK_NE(message1.MessageLength(), message2.MessageLength());  // Deserialisation captures the message length...
    BOOST_CHECK_EQUAL(9, message2.MessageLength());
    BOOST_CHECK_NE(message1.ChecksumValue(), message2.ChecksumValue());  // Deserialisation captures the message checksum value...
    BOOST_CHECK_EQUAL(0x26, message2.ChecksumValue());

    BOOST_CHECK_EQUAL(0x00, message1.SaltConcentrationPPM());
    BOOST_CHECK_EQUAL(0x00, message2.SaltConcentrationPPM());

    std::vector<uint8_t> message_bytes =
    {
        HEADER_BYTE_DLE,
        HEADER_BYTE_STX,
        0x00,
        magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
        0x00,
        magic_enum::enum_integer(AquariteStatuses::Unknown),
        0x00,
        HEADER_BYTE_DLE,
        HEADER_BYTE_ETX
    };

    message_bytes[4] = 0x00;
    message_bytes[5] = magic_enum::enum_integer(AquariteStatuses::On);
    {
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        BOOST_CHECK_EQUAL(0, message2.SaltConcentrationPPM());
        BOOST_CHECK_EQUAL(AquariteStatuses::On, message2.Status());
        const std::vector<AquariteStatuses> expected_flags{ AquariteStatuses::On };
        BOOST_TEST(message2.StatusFlags() == expected_flags, boost::test_tools::per_element());
    }

    message_bytes[4] = 0x1E;
    message_bytes[5] = magic_enum::enum_integer(AquariteStatuses::Warning_LowSalt);
    {
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        BOOST_CHECK_EQUAL(3000, message2.SaltConcentrationPPM());
        BOOST_CHECK_EQUAL(AquariteStatuses::Warning_LowSalt, message2.Status());
        const std::vector<AquariteStatuses> expected_flags{ AquariteStatuses::Warning_LowSalt };
        BOOST_TEST(message2.StatusFlags() == expected_flags, boost::test_tools::per_element());
    }

    message_bytes[4] = 0x28;
    message_bytes[5] = magic_enum::enum_integer(AquariteStatuses::TurningOff);
    {
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        BOOST_CHECK_EQUAL(4000, message2.SaltConcentrationPPM());
        BOOST_CHECK_EQUAL(AquariteStatuses::TurningOff, message2.Status());
        // TurningOff (0x09) bit-collides with Warning_NoFlow|Warning_CleanCell - this is
        // the case that proves exact-match-first ordering: it must NOT decompose.
        const std::vector<AquariteStatuses> expected_flags{ AquariteStatuses::TurningOff };
        BOOST_TEST(message2.StatusFlags() == expected_flags, boost::test_tools::per_element());
    }

    message_bytes[4] = 0xFF;
    message_bytes[5] = magic_enum::enum_integer(AquariteStatuses::Off);
    {
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        BOOST_REQUIRE(message2.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        BOOST_CHECK_EQUAL(25500, message2.SaltConcentrationPPM());
        BOOST_CHECK_EQUAL(AquariteStatuses::Off, message2.Status());
        // Off (0xFF) bit-collides with every warning flag ORed together - must NOT decompose.
        const std::vector<AquariteStatuses> expected_flags{ AquariteStatuses::Off };
        BOOST_TEST(message2.StatusFlags() == expected_flags, boost::test_tools::per_element());
    }
}

BOOST_AUTO_TEST_CASE(TestStatusFlags_CombinedByte_Decomposes)
{
    // Regression: a status byte that matches no single named AquariteStatuses value
    // used to fall back to Status() == Unknown and be silently lost downstream (the
    // dwell filter swallows Unknown readings). StatusFlags() must decompose it into
    // every individual flag it is made of instead.
    AquariteMessage_PPM message;

    std::vector<uint8_t> message_bytes =
    {
        HEADER_BYTE_DLE,
        HEADER_BYTE_STX,
        0x00,
        magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
        0x00,
        0x00,
        0x00,
        HEADER_BYTE_DLE,
        HEADER_BYTE_ETX
    };

    auto deserialize_status = [&](uint8_t raw_status) -> AquariteMessage_PPM
    {
        message_bytes[5] = raw_status;
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        AquariteMessage_PPM decoded;
        BOOST_REQUIRE(decoded.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        return decoded;
    };

    {
        // 0x06 = Warning_LowSalt(0x02) | Warning_HighSalt(0x04)
        auto decoded = deserialize_status(0x06);
        BOOST_CHECK_EQUAL(AquariteStatuses::Unknown, decoded.Status());
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::Warning_LowSalt, AquariteStatuses::Warning_HighSalt };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }

    {
        // 0x03 = Warning_NoFlow(0x01) | Warning_LowSalt(0x02)
        auto decoded = deserialize_status(0x03);
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::Warning_NoFlow, AquariteStatuses::Warning_LowSalt };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }

    {
        // 0x11 = Warning_NoFlow(0x01) | Warning_HighCurrent(0x10)
        auto decoded = deserialize_status(0x11);
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::Warning_NoFlow, AquariteStatuses::Warning_HighCurrent };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }

    {
        // 0x82 = Warning_LowSalt(0x02) | Error_CheckPCB(0x80)
        auto decoded = deserialize_status(0x82);
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::Warning_LowSalt, AquariteStatuses::Error_CheckPCB };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }
}

BOOST_AUTO_TEST_CASE(TestStatusFlags_SentinelBytes_NeverDecomposed)
{
    // GeneralFault (0xFD) and Unknown (0xFE) each bit-collide with a real 7-flag
    // combination, and Off (0xFF, covered in TestSerializationDeserialization above)
    // collides with all 8 flags at once. Exact-match-first ordering must intercept all
    // of these before decomposition ever runs, so they stay single-element.
    AquariteMessage_PPM message;

    std::vector<uint8_t> message_bytes =
    {
        HEADER_BYTE_DLE,
        HEADER_BYTE_STX,
        0x00,
        magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
        0x00,
        0x00,
        0x00,
        HEADER_BYTE_DLE,
        HEADER_BYTE_ETX
    };

    auto deserialize_status = [&](uint8_t raw_status) -> AquariteMessage_PPM
    {
        message_bytes[5] = raw_status;
        message_bytes[6] = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(message_bytes.begin(), message_bytes.begin() + 6);
        AquariteMessage_PPM decoded;
        BOOST_REQUIRE(decoded.Deserialize(std::as_bytes(std::span<uint8_t>(message_bytes))));
        return decoded;
    };

    {
        auto decoded = deserialize_status(0xFD);
        BOOST_CHECK_EQUAL(AquariteStatuses::GeneralFault, decoded.Status());
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::GeneralFault };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }

    {
        auto decoded = deserialize_status(0xFE);
        BOOST_CHECK_EQUAL(AquariteStatuses::Unknown, decoded.Status());
        const std::vector<AquariteStatuses> expected{ AquariteStatuses::Unknown };
        BOOST_TEST(decoded.StatusFlags() == expected, boost::test_tools::per_element());
    }
}

BOOST_AUTO_TEST_CASE(TestDeserialization_MessageMissingPayload)
{
    std::vector<std::vector<uint8_t>> message_bytes =
    {
        {
            HEADER_BYTE_DLE,
            HEADER_BYTE_STX,
            0x00,
            // 
            // The following would be the "expected" bytes...
            // 
            // magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
            // 0x00,
            // magic_enum::enum_integer(AquariteStatuses::Unknown),
            // <checksum>,
            // HEADER_BYTE_DLE,
            // HEADER_BYTE_ETX
        },
        {
            HEADER_BYTE_DLE,
            HEADER_BYTE_STX,
            0x00,
            magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
            // 
            // The following would be the "expected" bytes...
            // 
            // 0x00,
            // magic_enum::enum_integer(AquariteStatuses::Unknown),
            // <checksum>
            // HEADER_BYTE_DLE
            // HEADER_BYTE_ETX
        },
        {
            HEADER_BYTE_DLE,
            HEADER_BYTE_STX,
            0x00,
            magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
            0x00,
            // 
            // The following would be the "expected" bytes...
            // 
            // magic_enum::enum_integer(AquariteStatuses::Unknown),
            // <checksum>
            // HEADER_BYTE_DLE
            // HEADER_BYTE_ETX
        },
        {
            HEADER_BYTE_DLE,
            HEADER_BYTE_STX,
            0x00,
            magic_enum::enum_integer(JandyMessageIds::AQUARITE_PPM),
            0x00,
            magic_enum::enum_integer(AquariteStatuses::Unknown),
            // 
            // The following would be the "expected" bytes...
            // 
            // <checksum>
            // HEADER_BYTE_DLE
            // HEADER_BYTE_ETX
        }
    };

    AquariteMessage_PPM message;

    BOOST_CHECK_EQUAL(false, message.DeserializeContents(message_bytes[0]));
    BOOST_CHECK_EQUAL(false, message.DeserializeContents(message_bytes[1]));
    BOOST_CHECK_EQUAL(false, message.DeserializeContents(message_bytes[2]));
    BOOST_CHECK_EQUAL(true, message.DeserializeContents(message_bytes[3]));
}

BOOST_AUTO_TEST_CASE(TestToString)
{
    AquariteMessage_PPM message;

    std::string expected = "Packet: Destination: AqualinkMaster (0x00), Message Type: AQUARITE_PPM (0x16) || Payload: PPM: 0, Status: Unknown";

    BOOST_CHECK_EQUAL(message.ToString(), expected);
}

BOOST_AUTO_TEST_SUITE_END()
