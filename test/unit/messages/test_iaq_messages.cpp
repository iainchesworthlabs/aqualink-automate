#include <cstdint>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/messages/iaq/iaq_message_aux_status.h"
#include "jandy/messages/iaq/iaq_message_page_button.h"
#include "jandy/messages/iaq/iaq_message_page_message.h"
#include "jandy/messages/iaq/iaq_message_title_message.h"
#include "jandy/messages/iaq/iaq_message_table_message.h"
#include "jandy/messages/iaq/iaq_message_control_data_response.h"
#include "jandy/messages/iaq/iaq_message_heartbeat.h"
#include "jandy/messages/iaq/iaq_message_poll.h"
#include "jandy/messages/iaq/iaq_message_onetouch_status.h"

using namespace AqualinkAutomate::Messages;

// =============================================================================
// Helper: build a raw packet span for DeserializeContents
// The packet format is: [0x10, 0x02, dest, msg_type, ...data..., checksum, 0x10, 0x03]
// DeserializeContents receives the full framed bytes.
// =============================================================================

namespace
{
	std::vector<uint8_t> MakePacket(uint8_t dest, uint8_t msg_type, const std::vector<uint8_t>& payload)
	{
		std::vector<uint8_t> pkt;
		pkt.push_back(0x10); // DLE
		pkt.push_back(0x02); // STX
		pkt.push_back(dest);
		pkt.push_back(msg_type);
		pkt.insert(pkt.end(), payload.begin(), payload.end());
		pkt.push_back(0x00); // dummy checksum
		pkt.push_back(0x10); // DLE
		pkt.push_back(0x03); // ETX
		return pkt;
	}
}

// =============================================================================
// AuxStatus (0x72)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_AuxStatusTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_AuxStatus msg;
	BOOST_CHECK_EQUAL(msg.DeviceCount(), static_cast<uint8_t>(0));
	BOOST_CHECK(msg.Devices().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TwoDevices)
{
	// Payload: num_devices=2, indices=[3,5],
	// Device 0: status=0x01(ON), type=0x08, pad=0x00,0x00, name_len=11, name="Filter Pump"
	// Device 1: status=0x00(OFF), type=0x01, pad=0x00,0x00, name_len=8, name="Spa Mode"
	std::vector<uint8_t> payload = {
		0x02,       // num_devices
		0x03, 0x05, // device indices
		// Device 0
		0x01,       // status ON
		0x08,       // type
		0x00, 0x00, // pad
		0x0B,       // name_len = 11
		'F','i','l','t','e','r',' ','P','u','m','p',
		// Device 1
		0x00,       // status OFF
		0x01,       // type
		0x00, 0x00, // pad
		0x08,       // name_len = 8
		'S','p','a',' ','M','o','d','e'
	};
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK_EQUAL(msg.DeviceCount(), static_cast<uint8_t>(2));
	BOOST_REQUIRE(msg.Devices().size() == 2);

	BOOST_CHECK_EQUAL(msg.Devices()[0].device_index, static_cast<uint8_t>(3));
	BOOST_CHECK(msg.Devices()[0].is_on == true);
	BOOST_CHECK_EQUAL(msg.Devices()[0].type, static_cast<uint8_t>(0x08));
	BOOST_CHECK_EQUAL(msg.Devices()[0].name, "Filter Pump");

	BOOST_CHECK_EQUAL(msg.Devices()[1].device_index, static_cast<uint8_t>(5));
	BOOST_CHECK(msg.Devices()[1].is_on == false);
	BOOST_CHECK_EQUAL(msg.Devices()[1].type, static_cast<uint8_t>(0x01));
	BOOST_CHECK_EQUAL(msg.Devices()[1].name, "Spa Mode");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_ZeroDevices)
{
	std::vector<uint8_t> payload = { 0x00 }; // num_devices = 0
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK_EQUAL(msg.DeviceCount(), static_cast<uint8_t>(0));
	BOOST_CHECK(msg.Devices().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoPayload)
{
	// Empty payload -> packet size <= 4+3 = 7
	auto pkt = MakePacket(0x33, 0x72, {});
	IAQMessage_AuxStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForDeviceIndices)
{
	// num_devices=3 but only 1 index byte
	std::vector<uint8_t> payload = { 0x03, 0x01 };
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForDeviceHeader)
{
	// num_devices=1, index=[3], then only 2 bytes of header (need 5: status+type+pad+pad+name_len)
	std::vector<uint8_t> payload = { 0x01, 0x03, 0x01, 0x08 };
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForDeviceName)
{
	// num_devices=1, index=[3], header says name_len=10 but only 3 name bytes
	std::vector<uint8_t> payload = { 0x01, 0x03, 0x01, 0x08, 0x00, 0x00, 0x0A, 'A', 'B', 'C' };
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestToString_ContainsDeviceInfo)
{
	std::vector<uint8_t> payload = {
		0x01, 0x03,
		0x01, 0x08, 0x00, 0x00, 0x04, 'P','u','m','p'
	};
	auto pkt = MakePacket(0x33, 0x72, payload);
	IAQMessage_AuxStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	auto str = msg.ToString();
	BOOST_CHECK(str.find("Pump(ON)") != std::string::npos);
	BOOST_CHECK(str.find("1") != std::string::npos); // device count
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_AuxStatus msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// PageButton (0x24)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_PageButtonTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_PageButton msg;
	BOOST_CHECK_EQUAL(msg.ButtonIndex(), static_cast<uint8_t>(0));
	BOOST_CHECK(msg.ButtonStatus() == ButtonStatuses::Unknown);
	BOOST_CHECK(msg.ButtonType() == ButtonTypes::Unknown);
	BOOST_CHECK(msg.ButtonName().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_ValidButton)
{
	// index=3, status=On(0x01), unknown=0x00, type=Light(0x07), name="Pool Light"
	std::vector<uint8_t> payload = {
		0x03,       // ButtonIndex
		0x01,       // ButtonStatus = On
		0x00,       // unknown
		0x07,       // ButtonType = Light
		'P','o','o','l',' ','L','i','g','h','t'
	};
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK_EQUAL(msg.ButtonIndex(), static_cast<uint8_t>(3));
	BOOST_CHECK(msg.ButtonStatus() == ButtonStatuses::On);
	BOOST_CHECK(msg.ButtonType() == ButtonTypes::Light);
	BOOST_CHECK_EQUAL(msg.ButtonName(), "Pool Light");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_NulSeparatedLabelAndState)
{
	// Real capture (iaq_aux_setpoint.cap): the name payload is two NUL-separated
	// fields -- the label "Pool Heat" and its current state "OFF" -- followed by a
	// trailing NUL pad.  The interior NUL must render as a space (not '?') and the
	// trailing pad must be stripped, giving "Pool Heat OFF" rather than "Pool Heat?OFF?".
	std::vector<uint8_t> payload = {
		0x02,       // ButtonIndex
		0x00,       // ButtonStatus
		0x00,       // unknown
		0x01,       // ButtonType
		'P','o','o','l',' ','H','e','a','t', 0x00, 'O','F','F', 0x00
	};
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK_EQUAL(msg.ButtonName(), "Pool Heat OFF");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_InvalidEnumValues)
{
	// Unknown status 0x99 and type 0x99 -> both should map to Unknown
	std::vector<uint8_t> payload = {
		0x00,       // ButtonIndex
		0x99,       // ButtonStatus = invalid -> Unknown
		0x00,       // unknown
		0x99,       // ButtonType = invalid -> Unknown
		'T','e','s','t'
	};
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.ButtonStatus() == ButtonStatuses::Unknown);
	BOOST_CHECK(msg.ButtonType() == ButtonTypes::Unknown);
	BOOST_CHECK_EQUAL(msg.ButtonName(), "Test");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoButtonState)
{
	// Only header + 1 payload byte (Index_ButtonState = 5, need size > 5)
	std::vector<uint8_t> payload = { 0x03 };
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoButtonType)
{
	// Has status but not type (Index_ButtonType = 7, need size > 7)
	std::vector<uint8_t> payload = { 0x03, 0x01, 0x00 };
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_EmptyName)
{
	// Has type but no name text bytes -> succeeds with empty button name
	std::vector<uint8_t> payload = { 0x03, 0x01, 0x00, 0x07 };
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK(msg.ButtonName().empty());
	BOOST_CHECK_EQUAL(msg.ButtonIndex(), static_cast<uint8_t>(3));
	BOOST_CHECK(msg.ButtonType() == ButtonTypes::Light);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoNameContent)
{
	// Packet too short for content extraction (size < Index_ButtonNameText + 3)
	// 3-byte payload -> packet = 10 bytes, less than 8+3=11 minimum
	std::vector<uint8_t> payload = { 0x03, 0x01, 0x00 };
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_PageButton msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_CASE(TestToString_ContainsDecodedFields)
{
	// ToString() renders the button index, the enum names of status/type and the name.
	// index=3, status=On(0x01), type=Light(0x07), name="Pool Light".
	std::vector<uint8_t> payload = {
		0x03, 0x01, 0x00, 0x07,
		'P','o','o','l',' ','L','i','g','h','t'
	};
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	const auto str = msg.ToString();
	BOOST_CHECK(str.find("Index: 3") != std::string::npos);
	BOOST_CHECK(str.find("Status: On") != std::string::npos);
	BOOST_CHECK(str.find("Type: Light") != std::string::npos);
	BOOST_CHECK(str.find("Name: 'Pool Light'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestToString_RendersUnknownEnums)
{
	// Invalid status/type map to Unknown; ToString() must render "Unknown" for them.
	std::vector<uint8_t> payload = {
		0x00, 0x99, 0x00, 0x99,
		'T','e','s','t'
	};
	auto pkt = MakePacket(0x33, 0x24, payload);
	IAQMessage_PageButton msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	const auto str = msg.ToString();
	BOOST_CHECK(str.find("Status: Unknown") != std::string::npos);
	BOOST_CHECK(str.find("Type: Unknown") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// PageMessage (0x25)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_PageMessageTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_PageMessage msg;
	BOOST_CHECK_EQUAL(msg.LineId(), static_cast<uint8_t>(0));
	BOOST_CHECK(msg.Line().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_Valid)
{
	std::vector<uint8_t> payload = {
		0x05, // LineId
		'E','q','u','i','p','m','e','n','t',' ','S','t','a','t','u','s'
	};
	auto pkt = MakePacket(0x33, 0x25, payload);
	IAQMessage_PageMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK_EQUAL(msg.LineId(), static_cast<uint8_t>(5));
	BOOST_CHECK_EQUAL(msg.Line(), "Equipment Status");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TrailingNulPadStripped)
{
	// Real capture (iaq_aux_setpoint.cap): "Air Temp" followed by the panel's NUL pad.
	// The pad must not surface as "Air Temp?".
	std::vector<uint8_t> payload = {
		0x05, // LineId
		'A','i','r',' ','T','e','m','p', 0x00, 0x00
	};
	auto pkt = MakePacket(0x33, 0x25, payload);
	IAQMessage_PageMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK_EQUAL(msg.Line(), "Air Temp");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoLineId)
{
	auto pkt = MakePacket(0x33, 0x25, {});
	IAQMessage_PageMessage msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoLineText)
{
	// Only LineId, no text content (need size > Index_LineText=5 and size >= 5+3)
	std::vector<uint8_t> payload = { 0x05 };
	auto pkt = MakePacket(0x33, 0x25, payload);
	IAQMessage_PageMessage msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_PageMessage msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// TitleMessage (0x2D)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_TitleMessageTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_TitleMessage msg;
	BOOST_CHECK(msg.Title().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_Valid)
{
	std::vector<uint8_t> payload = {
		'S','y','s','t','e','m',' ','S','e','t','u','p'
	};
	auto pkt = MakePacket(0x33, 0x2D, payload);
	IAQMessage_TitleMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK_EQUAL(msg.Title(), "System Setup");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_CentredTitleTrailingNulPad)
{
	// A centred title: leading spaces position the word and a trailing NUL pad fills the
	// cell.  The leading spaces (centring) must survive; the trailing pad must not become
	// "?".
	std::vector<uint8_t> payload = {
		' ',' ',' ','H','o','m','e', 0x00, 0x00, 0x00
	};
	auto pkt = MakePacket(0x33, 0x2D, payload);
	IAQMessage_TitleMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK_EQUAL(msg.Title(), "   Home");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort)
{
	auto pkt = MakePacket(0x33, 0x2D, {});
	IAQMessage_TitleMessage msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestToString_ContainsTitle)
{
	std::vector<uint8_t> payload = { 'H','o','m','e' };
	auto pkt = MakePacket(0x33, 0x2D, payload);
	IAQMessage_TitleMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	auto str = msg.ToString();
	BOOST_CHECK(str.find("Home") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_TitleMessage msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// TableMessage (0x26)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_TableMessageTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_TableMessage msg;
	BOOST_CHECK_EQUAL(msg.LineId(), static_cast<uint8_t>(0));
	BOOST_CHECK(msg.Line().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_Valid)
{
	// 0x26 carries TWO leading bytes (line + attribute) before the text -- unlike
	// 0x25 which has one. The attribute byte must NOT bleed into the line text.
	std::vector<uint8_t> payload = {
		0x02, // LineId
		0x01, // Attribute
		'T','a','b','l','e',' ','R','o','w'
	};
	auto pkt = MakePacket(0x33, 0x26, payload);
	IAQMessage_TableMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK_EQUAL(msg.LineId(), static_cast<uint8_t>(2));
	BOOST_CHECK_EQUAL(msg.Attribute(), static_cast<uint8_t>(1));
	BOOST_CHECK_EQUAL(msg.Line(), "Table Row");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TrailingNulPadStripped)
{
	// A table row padded with trailing NUL must decode without "?" junk.
	std::vector<uint8_t> payload = {
		0x02, // LineId
		0x01, // Attribute
		'R','o','w', 0x00, 0x00
	};
	auto pkt = MakePacket(0x33, 0x26, payload);
	IAQMessage_TableMessage msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK_EQUAL(msg.Line(), "Row");
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoLineId)
{
	auto pkt = MakePacket(0x33, 0x26, {});
	IAQMessage_TableMessage msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoLineText)
{
	// Line + attribute present, but no text byte -> rejected.
	std::vector<uint8_t> payload = { 0x02, 0x01 };
	auto pkt = MakePacket(0x33, 0x26, payload);
	IAQMessage_TableMessage msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_TableMessage msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// ControlDataResponse (send-only)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_ControlDataResponseTestSuite)

BOOST_AUTO_TEST_CASE(TestConstruction_WithData)
{
	IAQMessage_ControlDataResponse msg("175");
	auto str = msg.ToString();
	BOOST_CHECK(str.find("175") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestSerialize_FullString)
{
	IAQMessage_ControlDataResponse msg("175");
	std::vector<uint8_t> out;
	BOOST_REQUIRE(msg.SerializeContents(out));
	BOOST_CHECK_EQUAL(out.size(), static_cast<size_t>(17));
	BOOST_CHECK_EQUAL(out[0], static_cast<uint8_t>('1'));
	BOOST_CHECK_EQUAL(out[1], static_cast<uint8_t>('7'));
	BOOST_CHECK_EQUAL(out[2], static_cast<uint8_t>('5'));
	// Remaining bytes should be 0x00 padding
	for (size_t i = 3; i < 17; ++i)
	{
		BOOST_CHECK_EQUAL(out[i], static_cast<uint8_t>(0x00));
	}
}

BOOST_AUTO_TEST_CASE(TestSerialize_ShortString)
{
	IAQMessage_ControlDataResponse msg("AB");
	std::vector<uint8_t> out;
	BOOST_REQUIRE(msg.SerializeContents(out));
	BOOST_CHECK_EQUAL(out.size(), static_cast<size_t>(17));
	BOOST_CHECK_EQUAL(out[0], static_cast<uint8_t>('A'));
	BOOST_CHECK_EQUAL(out[1], static_cast<uint8_t>('B'));
	for (size_t i = 2; i < 17; ++i)
	{
		BOOST_CHECK_EQUAL(out[i], static_cast<uint8_t>(0x00));
	}
}

BOOST_AUTO_TEST_CASE(TestSerialize_EmptyString)
{
	IAQMessage_ControlDataResponse msg("");
	std::vector<uint8_t> out;
	BOOST_REQUIRE(msg.SerializeContents(out));
	BOOST_CHECK_EQUAL(out.size(), static_cast<size_t>(17));
	for (size_t i = 0; i < 17; ++i)
	{
		BOOST_CHECK_EQUAL(out[i], static_cast<uint8_t>(0x00));
	}
}

BOOST_AUTO_TEST_CASE(TestSerialize_LongString_Truncated)
{
	IAQMessage_ControlDataResponse msg("12345678901234567890"); // 20 chars > 17
	std::vector<uint8_t> out;
	BOOST_REQUIRE(msg.SerializeContents(out));
	BOOST_CHECK_EQUAL(out.size(), static_cast<size_t>(17));
	// First 17 chars should be written
	for (size_t i = 0; i < 17; ++i)
	{
		BOOST_CHECK_EQUAL(out[i], static_cast<uint8_t>("12345678901234567"[i]));
	}
}

BOOST_AUTO_TEST_CASE(TestDeserialize_ReturnsFalse)
{
	IAQMessage_ControlDataResponse msg("test");
	auto pkt = MakePacket(0x00, 0x24, { 0x01, 0x02, 0x03 });
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Heartbeat (0x53)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_HeartbeatTestSuite)

BOOST_AUTO_TEST_CASE(TestDeserialize_AlwaysTrue)
{
	IAQMessage_Heartbeat msg;
	auto pkt = MakePacket(0xA3, 0x53, {});
	BOOST_CHECK(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_WithPayload_AlwaysTrue)
{
	IAQMessage_Heartbeat msg;
	auto pkt = MakePacket(0xA3, 0x53, { 0x01, 0x02, 0x03 });
	BOOST_CHECK(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_Heartbeat msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Poll (0x30)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_PollTestSuite)

BOOST_AUTO_TEST_CASE(TestDeserialize_AlwaysTrue)
{
	IAQMessage_Poll msg;
	auto pkt = MakePacket(0x33, 0x30, {});
	BOOST_CHECK(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_Poll msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// OneTouchStatus (0x71)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_OneTouchStatusTestSuite)

BOOST_AUTO_TEST_CASE(TestDeserialize_EmptyPayload)
{
	IAQMessage_OneTouchStatus msg;
	auto pkt = MakePacket(0x33, 0x71, {});
	// Always returns true, but with empty payload (size <= 7)
	BOOST_CHECK(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	BOOST_CHECK(msg.RawPayload().empty());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_WithPayload)
{
	std::vector<uint8_t> payload = { 0xAA, 0xBB, 0xCC, 0xDD };
	auto pkt = MakePacket(0x33, 0x71, payload);
	IAQMessage_OneTouchStatus msg;
	BOOST_CHECK(msg.DeserializeContents(std::span<const uint8_t>(pkt)));
	// RawPayload should be the bytes between header(4) and footer(3)
	BOOST_CHECK_EQUAL(msg.RawPayload().size(), static_cast<size_t>(4));
	BOOST_CHECK_EQUAL(msg.RawPayload()[0], static_cast<uint8_t>(0xAA));
	BOOST_CHECK_EQUAL(msg.RawPayload()[1], static_cast<uint8_t>(0xBB));
	BOOST_CHECK_EQUAL(msg.RawPayload()[2], static_cast<uint8_t>(0xCC));
	BOOST_CHECK_EQUAL(msg.RawPayload()[3], static_cast<uint8_t>(0xDD));
}

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_OneTouchStatus msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()
