#include <cstdint>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/messages/serial_adapter/serial_adapter_message_dev_ready.h"
#include "jandy/messages/serial_adapter/serial_adapter_message_dev_status.h"

using namespace AqualinkAutomate::Messages;

namespace
{
	std::vector<uint8_t> MakePacket(uint8_t dest, uint8_t msg_type, const std::vector<uint8_t>& payload)
	{
		std::vector<uint8_t> pkt;
		pkt.push_back(0x10); pkt.push_back(0x02);
		pkt.push_back(dest); pkt.push_back(msg_type);
		pkt.insert(pkt.end(), payload.begin(), payload.end());
		pkt.push_back(0x00); pkt.push_back(0x10); pkt.push_back(0x03);
		return pkt;
	}
}

// =============================================================================
// SerialAdapterMessage_DevReady
// =============================================================================

BOOST_AUTO_TEST_SUITE(SerialAdapterMessage_DevReady_TestSuite)

BOOST_AUTO_TEST_CASE(TestDeserialize_AlwaysReturnsTrue)
{
	SerialAdapterMessage_DevReady msg;
	auto pkt = MakePacket(0x48, 0x40, {});
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));
	BOOST_CHECK(result);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_EmptySpan_ReturnsTrue)
{
	SerialAdapterMessage_DevReady msg;
	std::vector<uint8_t> empty;
	auto result = msg.DeserializeContents(std::span<const uint8_t>(empty));
	BOOST_CHECK(result);
}

BOOST_AUTO_TEST_CASE(TestSerialize_ProducesValidPacket)
{
	SerialAdapterMessage_DevReady msg;
	std::vector<uint8_t> bytes;
	BOOST_CHECK(msg.SerializeContents(bytes));

	// Expected: [DLE, STX, 0x00, msg_type, 0x00, checksum, DLE, ETX]
	BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
	BOOST_CHECK_EQUAL(bytes[0], 0x10); // DLE
	BOOST_CHECK_EQUAL(bytes[1], 0x02); // STX
	BOOST_CHECK_EQUAL(bytes[2], 0x00); // dest
	BOOST_CHECK_EQUAL(bytes[6], 0x10); // DLE
	BOOST_CHECK_EQUAL(bytes[7], 0x03); // ETX

	// Checksum: sum of bytes[0..4] & 0xFF
	uint8_t expected_checksum = 0;
	for (int i = 0; i < 5; ++i)
	{
		expected_checksum += bytes[i];
	}
	BOOST_CHECK_EQUAL(bytes[5], expected_checksum);
}

BOOST_AUTO_TEST_CASE(TestToString_NotEmpty)
{
	SerialAdapterMessage_DevReady msg;
	auto str = msg.ToString();
	BOOST_CHECK(!str.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// SerialAdapterMessage_DevStatus
// =============================================================================

BOOST_AUTO_TEST_SUITE(SerialAdapterMessage_DevStatus_TestSuite)

// --- Construction ---

BOOST_AUTO_TEST_CASE(TestConstruction_Default)
{
	SerialAdapterMessage_DevStatus msg;
	BOOST_CHECK(!msg.ModelType().has_value());
	BOOST_CHECK(!msg.OpMode().has_value());
	BOOST_CHECK(!msg.Options().has_value());
	BOOST_CHECK(!msg.BatteryCondition().has_value());
	BOOST_CHECK(!msg.TemperatureUnits().has_value());
	BOOST_CHECK(!msg.Pool_SetPoint_One().has_value());
	BOOST_CHECK(!msg.Spa_SetPoint().has_value());
	BOOST_CHECK(!msg.AirTemperature().has_value());
	BOOST_CHECK(!msg.PoolTemperature().has_value());
	BOOST_CHECK(!msg.SolarTemperature().has_value());
	BOOST_CHECK(!msg.SpaTemperature().has_value());
	BOOST_CHECK(!msg.AuxilliaryState().has_value());
}

BOOST_AUTO_TEST_CASE(TestConstruction_WithSCS)
{
	SerialAdapterMessage_DevStatus msg(SerialAdapter_SystemConfigurationStatuses::MODEL);
	// Should construct without error
	BOOST_CHECK(!msg.ModelType().has_value());
}

BOOST_AUTO_TEST_CASE(TestConstruction_WithSTC)
{
	SerialAdapterMessage_DevStatus msg(SerialAdapter_SystemTemperatureCommands::UNITS);
	BOOST_CHECK(!msg.TemperatureUnits().has_value());
}

// --- Deserialize: MODEL (SCS, StatusType=0x00) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_Model)
{
	SerialAdapterMessage_DevStatus msg;
	// Index_StatusType=4, value=0x00 (MODEL enum), Index_ModelType_Part1=5, Index_ModelType_Part2=6
	// Model = (part1 << 8) + part2 = (0x01 << 8) + 0x23 = 291
	auto pkt = MakePacket(0x48, 0x41, { 0x00, 0x01, 0x23, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.ModelType().has_value());
	BOOST_CHECK_EQUAL(msg.ModelType().value(), 291);
}

// --- Deserialize: OPMODE (SCS, StatusType=0x0D) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_OpMode_Auto)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x0D (OPMODE), Index_OpMode=6
	auto pkt = MakePacket(0x48, 0x41, { 0x0D, 0x00, 0x00, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.OpMode().has_value());
	BOOST_CHECK(msg.OpMode().value() == SerialAdapter_SCS_OpModes::Auto);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_OpMode_Service)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x0D, 0x00, 0x01, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.OpMode().has_value());
	BOOST_CHECK(msg.OpMode().value() == SerialAdapter_SCS_OpModes::Service);
}

// --- Deserialize: OPTIONS (SCS, StatusType=0x01) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_Options_HasCleaner)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x01 (OPTIONS), Index_Options=6
	// Byte 0x01 = bit 0 set → HasCleaner=true
	auto pkt = MakePacket(0x48, 0x41, { 0x01, 0x00, 0x01, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Options().has_value());
	BOOST_CHECK(msg.Options().value().HasCleaner);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_Options_AllZero)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x01, 0x00, 0x00, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Options().has_value());
	auto opts = msg.Options().value();
	BOOST_CHECK(!opts.HasCleaner);
	BOOST_CHECK(!opts.TwoSpeedPump);
	BOOST_CHECK(!opts.HasSpillover);
}

// --- Deserialize: VBAT (SCS, StatusType=0x0E) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_BatteryCondition)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x0E (VBAT)
	// IsLow = byte[5] & 0x04. voltage_bits = ((byte[5] & 0x03) << 8) + byte[6]
	// byte[5]=0x05 → IsLow = (0x05 & 0x04) = true, voltage_bits_part1 = (0x05 & 0x03) << 8 = 0x100
	// byte[6]=0x50 → voltage_bits = 0x100 + 0x50 = 336, voltage = 336/100 = 3.36V
	auto pkt = MakePacket(0x48, 0x41, { 0x0E, 0x05, 0x50, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.BatteryCondition().has_value());
	BOOST_CHECK(msg.BatteryCondition().value().IsLow);
}

// --- Deserialize: UNITS (STC, StatusType=0x0A) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_TemperatureUnits_Fahrenheit)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x0A (UNITS), Index_TemperatureUnits=6, 0x00=Fahrenheit
	auto pkt = MakePacket(0x48, 0x41, { 0x0A, 0x00, 0x00, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.TemperatureUnits().has_value());
	BOOST_CHECK(msg.TemperatureUnits().value() == AqualinkAutomate::Kernel::TemperatureUnits::Fahrenheit);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TemperatureUnits_Celsius)
{
	SerialAdapterMessage_DevStatus msg;
	// 0x01 → Celsius
	auto pkt = MakePacket(0x48, 0x41, { 0x0A, 0x00, 0x01, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.TemperatureUnits().has_value());
	BOOST_CHECK(msg.TemperatureUnits().value() == AqualinkAutomate::Kernel::TemperatureUnits::Celsius);
}

// --- Deserialize: POOLSP (STC, StatusType=0x05) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_PoolSetPointOne)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x05, 0x00, 82, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Pool_SetPoint_One().has_value());
	BOOST_CHECK_EQUAL(msg.Pool_SetPoint_One().value(), 82);
}

// --- Deserialize: POOLSP2 (STC, StatusType=0x06) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_PoolSetPointTwo)
{
	// POOLSP2 == the panel's "TEMP2" maintenance setpoint (single-body systems).
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x06, 0x00, 78, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Pool_SetPoint_Two().has_value());
	BOOST_CHECK_EQUAL(msg.Pool_SetPoint_Two().value(), 78);
}

// --- Deserialize: POOLHT2 enable (STC, StatusType=0x12) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_PoolHeaterTwo_Enabled)
{
	// POOLHT2 == TEMP2 maintenance heating enable. Value byte (index 6 / payload[2]) non-zero.
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x12, 0x00, 0x01, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Pool_Heater_Two_Enabled().has_value());
	BOOST_CHECK_EQUAL(msg.Pool_Heater_Two_Enabled().value(), true);
}

BOOST_AUTO_TEST_CASE(TestDeserialize_PoolHeaterTwo_Disabled)
{
	// Value byte (index 6) of 0x00 -> heating disabled (proves we read the value, not the selector).
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x12, 0x00, 0x00, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Pool_Heater_Two_Enabled().has_value());
	BOOST_CHECK_EQUAL(msg.Pool_Heater_Two_Enabled().value(), false);
}

// --- Deserialize: SPASP (STC, StatusType=0x07) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_SpaSetPoint)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x07, 0x00, 100, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.Spa_SetPoint().has_value());
	BOOST_CHECK_EQUAL(msg.Spa_SetPoint().value(), 100);
}

// --- Deserialize: POOLTMP (STC, StatusType=0x08) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_PoolTemperature)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x08, 0x00, 78, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.PoolTemperature().has_value());
	BOOST_CHECK_EQUAL(msg.PoolTemperature().value(), 78);
}

// --- Deserialize: AIRTMP (STC, StatusType=0x09) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_AirTemperature)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x09, 0x00, 72, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.AirTemperature().has_value());
	BOOST_CHECK_EQUAL(msg.AirTemperature().value(), 72);
}

// --- Deserialize: SPATMP (STC, StatusType=0x0B) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_SpaTemperature)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x0B, 0x00, 102, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.SpaTemperature().has_value());
	BOOST_CHECK_EQUAL(msg.SpaTemperature().value(), 102);
}

// --- Deserialize: SOLTMP (STC, StatusType=0x0C) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_SolarTemperature)
{
	SerialAdapterMessage_DevStatus msg;
	auto pkt = MakePacket(0x48, 0x41, { 0x0C, 0x00, 110, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.SolarTemperature().has_value());
	BOOST_CHECK_EQUAL(msg.SolarTemperature().value(), 110);
}

// --- Deserialize: Auxillary via 0x03 status type ---

BOOST_AUTO_TEST_CASE(TestDeserialize_AuxillaryState)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x03 → HandleResponseAboutDevice
	// Index_DeviceId=7, value = JandyAuxillaryIds::Aux_1(0x01) + SERIALADAPTER_AUX_ID_OFFSET(0x14) = 0x15
	// Index_AuxState=6, JandyAuxillaryStatuses::On = 0x01
	auto pkt = MakePacket(0x48, 0x41, { 0x03, 0x00, 0x01, 0x15 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	BOOST_REQUIRE(msg.AuxilliaryState().has_value());
	auto [aux_id, aux_status] = msg.AuxilliaryState().value();
	BOOST_CHECK(aux_id == AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_1);
	BOOST_REQUIRE(aux_status.has_value());
	BOOST_CHECK(aux_status.value() == AqualinkAutomate::Auxillaries::JandyAuxillaryStatuses::On);
}

// --- Deserialize: Error status (0x02) ---

BOOST_AUTO_TEST_CASE(TestDeserialize_ErrorStatus)
{
	SerialAdapterMessage_DevStatus msg;
	// StatusType=0x02 → ErrorOccurred
	auto pkt = MakePacket(0x48, 0x41, { 0x02, 0x00, 0x00, 0x00 });
	auto result = msg.DeserializeContents(std::span<const uint8_t>(pkt));

	BOOST_CHECK(result);
	// Error status is stored as variant, not in optional fields
	// Just verify it deserialized without issue
}

// --- Deserialize: Too short ---

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort)
{
	SerialAdapterMessage_DevStatus msg;
	// size <= Index_StatusType(4) → need <=4 bytes
	std::vector<uint8_t> data = { 0x10, 0x02, 0x48, 0x41 };
	auto result = msg.DeserializeContents(std::span<const uint8_t>(data));
	BOOST_CHECK(!result);
}

// --- Serialize ---

BOOST_AUTO_TEST_CASE(TestSerialize_SCS_MODEL)
{
	SerialAdapterMessage_DevStatus msg(SerialAdapter_SystemConfigurationStatuses::MODEL);
	std::vector<uint8_t> bytes;
	BOOST_CHECK(msg.SerializeContents(bytes));

	BOOST_REQUIRE_EQUAL(bytes.size(), 11u);
	BOOST_CHECK_EQUAL(bytes[0], 0x10); // DLE
	BOOST_CHECK_EQUAL(bytes[1], 0x02); // STX
	BOOST_CHECK_EQUAL(bytes[4], 0x05); // Query command type
	BOOST_CHECK_EQUAL(bytes[5], 0x00); // MODEL = 0x00
	BOOST_CHECK_EQUAL(bytes[9], 0x10); // DLE
	BOOST_CHECK_EQUAL(bytes[10], 0x03); // ETX
}

BOOST_AUTO_TEST_CASE(TestSerialize_STC_UNITS)
{
	SerialAdapterMessage_DevStatus msg(SerialAdapter_SystemTemperatureCommands::UNITS);
	std::vector<uint8_t> bytes;
	BOOST_CHECK(msg.SerializeContents(bytes));

	BOOST_REQUIRE_EQUAL(bytes.size(), 11u);
	BOOST_CHECK_EQUAL(bytes[4], 0x05); // Query command type
	BOOST_CHECK_EQUAL(bytes[5], 0x0A); // UNITS = 0x0A
}

BOOST_AUTO_TEST_CASE(TestSerialize_AuxillaryId)
{
	SerialAdapterMessage_DevStatus msg(AqualinkAutomate::Auxillaries::JandyAuxillaryIds::Aux_1);
	std::vector<uint8_t> bytes;
	BOOST_CHECK(msg.SerializeContents(bytes));

	BOOST_REQUIRE_EQUAL(bytes.size(), 11u);
	BOOST_CHECK_EQUAL(bytes[4], 0x00); // Aux command type
	// Aux_1 = 0x01 + SERIALADAPTER_AUX_ID_OFFSET(0x14) = 0x15
	BOOST_CHECK_EQUAL(bytes[5], 0x15);
}

BOOST_AUTO_TEST_CASE(TestSerialize_HasValidChecksum)
{
	SerialAdapterMessage_DevStatus msg(SerialAdapter_SystemConfigurationStatuses::MODEL);
	std::vector<uint8_t> bytes;
	msg.SerializeContents(bytes);

	// Checksum is at bytes[8], covers bytes[0..7]
	uint8_t expected = 0;
	for (int i = 0; i < 8; ++i)
	{
		expected += bytes[i];
	}
	BOOST_CHECK_EQUAL(bytes[8], expected);
}

BOOST_AUTO_TEST_CASE(TestToString_NotEmpty)
{
	SerialAdapterMessage_DevStatus msg;
	auto str = msg.ToString();
	BOOST_CHECK(!str.empty());
}

BOOST_AUTO_TEST_SUITE_END()
