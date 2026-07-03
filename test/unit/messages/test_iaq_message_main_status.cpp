#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/messages/iaq/iaq_message_main_status.h"

using namespace AqualinkAutomate::Messages;
using namespace AqualinkAutomate::Kernel;

// =============================================================================
// Helper: build a raw packet span for DeserializeContents
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

	// Helper to encode a temperature as BE uint16
	void PushTempBE(std::vector<uint8_t>& v, uint16_t raw_value)
	{
		v.push_back(static_cast<uint8_t>(raw_value >> 8));
		v.push_back(static_cast<uint8_t>(raw_value & 0xFF));
	}

	bool ApproxEqual(double a, double b, double tolerance = 0.15)
	{
		return std::fabs(a - b) < tolerance;
	}
}

// =============================================================================
// MainStatus (0x70)
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQMessage_MainStatusTestSuite)

// =============================================================================
// Construction defaults
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_Defaults)
{
	IAQMessage_MainStatus msg;
	BOOST_CHECK(msg.PumpOn() == false);
	BOOST_CHECK(msg.SpaMode() == false);
	BOOST_CHECK(!msg.PoolTemperature().has_value());
	BOOST_CHECK(!msg.SpaTemperature().has_value());
	BOOST_CHECK(!msg.AirTemperature().has_value());
	BOOST_CHECK(!msg.PoolSetpoint().has_value());
	BOOST_CHECK(!msg.SpaSetpoint().has_value());
	BOOST_CHECK(msg.PoolHeaterStatus() == HeaterStatuses::Unknown);
	BOOST_CHECK(msg.SpaHeaterStatus() == HeaterStatuses::Unknown);
	BOOST_CHECK(msg.SolarHeaterStatus() == HeaterStatuses::Unknown);
	BOOST_CHECK(!msg.HeaterSetpoint().has_value());
	BOOST_CHECK(msg.DeviceIds().empty());
}

// =============================================================================
// Legacy format (with 0x0e 0x0f sentinel)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDeserialize_LegacyFormat_ThreeDevices_PumpOn_SpaOff)
{
	// Legacy: 3 devices + sentinel + pump ON + spa OFF + skip(2) + temps in deci-Celsius BE
	// Pool = 28.5C -> 285 = 0x011D, Spa = 30.0C -> 300 = 0x012C, Air = 25.0C -> 250 = 0x00FA
	std::vector<uint8_t> payload;
	payload.push_back(0x03);       // device_count = 3
	payload.push_back(0x01);       // device ID 0x01
	payload.push_back(0x02);       // device ID 0x02
	payload.push_back(0x08);       // device ID 0x08
	payload.push_back(0x0E);       // sentinel byte 1
	payload.push_back(0x0F);       // sentinel byte 2
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);       // spa OFF
	payload.push_back(0x00);       // unknown1
	payload.push_back(0x00);       // unknown2
	PushTempBE(payload, 285);      // pool = 28.5C
	PushTempBE(payload, 300);      // spa = 30.0C
	PushTempBE(payload, 250);      // air = 25.0C

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.PumpOn() == true);
	BOOST_CHECK(msg.SpaMode() == false);
	BOOST_REQUIRE(msg.PoolTemperature().has_value());
	BOOST_REQUIRE(msg.SpaTemperature().has_value());
	BOOST_REQUIRE(msg.AirTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.PoolTemperature()->InCelsius().value(), 28.5));
	BOOST_CHECK(ApproxEqual(msg.SpaTemperature()->InCelsius().value(), 30.0));
	BOOST_CHECK(ApproxEqual(msg.AirTemperature()->InCelsius().value(), 25.0));

	BOOST_REQUIRE(msg.DeviceIds().size() == 3);
	BOOST_CHECK_EQUAL(msg.DeviceIds()[0], static_cast<uint8_t>(0x01));
	BOOST_CHECK_EQUAL(msg.DeviceIds()[1], static_cast<uint8_t>(0x02));
	BOOST_CHECK_EQUAL(msg.DeviceIds()[2], static_cast<uint8_t>(0x08));

	// No heater setpoint (no extra bytes after temps)
	BOOST_CHECK(!msg.HeaterSetpoint().has_value());
}

BOOST_AUTO_TEST_CASE(TestDeserialize_LegacyFormat_WithHeaterSetpoint)
{
	// Same as above but with 2 extra bytes for heater setpoint: 27.0C -> 270 = 0x010E
	std::vector<uint8_t> payload;
	payload.push_back(0x02);       // device_count = 2
	payload.push_back(0x01);
	payload.push_back(0x02);
	payload.push_back(0x0E);       // sentinel
	payload.push_back(0x0F);
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x01);       // spa ON
	payload.push_back(0x00);       // unknown
	payload.push_back(0x00);       // unknown
	PushTempBE(payload, 280);      // pool = 28.0C
	PushTempBE(payload, 310);      // spa = 31.0C
	PushTempBE(payload, 220);      // air = 22.0C
	PushTempBE(payload, 270);      // heater setpoint = 27.0C

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.PumpOn() == true);
	BOOST_CHECK(msg.SpaMode() == true);
	BOOST_REQUIRE(msg.HeaterSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.HeaterSetpoint()->InCelsius().value(), 27.0));
}

// =============================================================================
// Current format (no sentinel, RS-8 Combo style)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDeserialize_CurrentFormat_FiveDevices)
{
	// Current format: 5 devices (including 0x0e, 0x0f as valid IDs) + status flags + temps in whole degrees
	// pump ON, pool_heater=Heating(0x01), spa OFF, spa_heater=Enabled(0x03), solar=Off(0x00)
	// pool_target=28C, spa_target=32C, air=24C, water_current=27C
	std::vector<uint8_t> payload;
	payload.push_back(0x05);       // device_count = 5
	payload.push_back(0x01);       // device IDs
	payload.push_back(0x02);
	payload.push_back(0x08);
	payload.push_back(0x0E);       // 0x0e as valid device ID (not sentinel)
	payload.push_back(0x0F);       // 0x0f as valid device ID (not sentinel)
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x01);       // pool heater = Heating
	payload.push_back(0x00);       // spa OFF
	payload.push_back(0x03);       // spa heater = Enabled
	payload.push_back(0x00);       // solar = Off
	PushTempBE(payload, 28);       // pool_target = 28C
	PushTempBE(payload, 32);       // spa_target = 32C
	PushTempBE(payload, 24);       // air = 24C
	PushTempBE(payload, 27);       // water_current = 27C

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.PumpOn() == true);
	BOOST_CHECK(msg.SpaMode() == false);

	BOOST_CHECK(msg.PoolHeaterStatus() == HeaterStatuses::Heating);
	BOOST_CHECK(msg.SpaHeaterStatus() == HeaterStatuses::Enabled);
	BOOST_CHECK(msg.SolarHeaterStatus() == HeaterStatuses::Off);

	// Current format: pool_target/spa_target are SETPOINTS; water_current is the
	// actual temperature of the active body (pool here, since spa mode is off).
	BOOST_REQUIRE(msg.PoolSetpoint().has_value());
	BOOST_REQUIRE(msg.SpaSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.PoolSetpoint()->InCelsius().value(), 28.0));
	BOOST_CHECK(ApproxEqual(msg.SpaSetpoint()->InCelsius().value(), 32.0));

	BOOST_REQUIRE(msg.PoolTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.PoolTemperature()->InCelsius().value(), 27.0));
	BOOST_CHECK(!msg.SpaTemperature().has_value());   // spa not circulating -> no reading
	BOOST_REQUIRE(msg.AirTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.AirTemperature()->InCelsius().value(), 24.0));

	// HeaterSetpoint = pool_target when not in spa mode
	BOOST_REQUIRE(msg.HeaterSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.HeaterSetpoint()->InCelsius().value(), 28.0));

	BOOST_REQUIRE(msg.DeviceIds().size() == 5);
	BOOST_CHECK_EQUAL(msg.DeviceIds()[3], static_cast<uint8_t>(0x0E));
	BOOST_CHECK_EQUAL(msg.DeviceIds()[4], static_cast<uint8_t>(0x0F));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_CurrentFormat_SpaMode_HeaterSetpoint)
{
	// In spa mode, HeaterSetpoint should be spa_target
	std::vector<uint8_t> payload;
	payload.push_back(0x03);       // device_count = 3
	payload.push_back(0x01);
	payload.push_back(0x02);
	payload.push_back(0x03);
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);       // pool heater = Off
	payload.push_back(0x01);       // spa ON
	payload.push_back(0x01);       // spa heater = Heating
	payload.push_back(0x00);       // solar = Off
	PushTempBE(payload, 28);       // pool_target
	PushTempBE(payload, 35);       // spa_target
	PushTempBE(payload, 22);       // air
	PushTempBE(payload, 34);       // water_current

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.SpaMode() == true);
	// HeaterSetpoint = spa_target when in spa mode
	BOOST_REQUIRE(msg.HeaterSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.HeaterSetpoint()->InCelsius().value(), 35.0));

	// In spa mode, water_current is the SPA's actual temperature; the pool has no reading.
	BOOST_REQUIRE(msg.SpaTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.SpaTemperature()->InCelsius().value(), 34.0));
	BOOST_CHECK(!msg.PoolTemperature().has_value());
}

// =============================================================================
// Regression: the pool/spa TARGETS must never be reported as measured
// temperatures, and a pump-off (or sentinel) water_current must yield NO
// reading rather than a fabricated one.  Previously PoolTemperature() returned
// pool_target, so with the pump off the UI showed the setpoint (e.g. 30C) as a
// live pool temperature instead of flagging the reading as stale.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDeserialize_CurrentFormat_PumpOff_NoWaterTemperature)
{
	// Pump OFF: water_current carries the 0x0001 "no reading" sentinel.
	std::vector<uint8_t> payload;
	payload.push_back(0x00);       // device_count = 0
	payload.push_back(0x00);       // pump OFF
	payload.push_back(0x00);       // pool heater = Off
	payload.push_back(0x00);       // spa OFF
	payload.push_back(0x00);       // spa heater = Off
	payload.push_back(0x00);       // solar = Off
	PushTempBE(payload, 30);       // pool_target = 30C (the user-visible setpoint)
	PushTempBE(payload, 39);       // spa_target = 39C
	PushTempBE(payload, 24);       // air = 24C
	PushTempBE(payload, 1);        // water_current = 0x0001 sentinel (pump off)

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	// No water reading — and in particular NOT the 30C setpoint.
	BOOST_CHECK(!msg.PoolTemperature().has_value());
	BOOST_CHECK(!msg.SpaTemperature().has_value());

	// The setpoints and air temperature are still decoded.
	BOOST_REQUIRE(msg.PoolSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.PoolSetpoint()->InCelsius().value(), 30.0));
	BOOST_REQUIRE(msg.SpaSetpoint().has_value());
	BOOST_CHECK(ApproxEqual(msg.SpaSetpoint()->InCelsius().value(), 39.0));
	BOOST_REQUIRE(msg.AirTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.AirTemperature()->InCelsius().value(), 24.0));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_CurrentFormat_SentinelWithPumpOn_NoWaterTemperature)
{
	// Defensive: even with the pump flagged ON, a sentinel water_current (<= 0x0001)
	// must not be reported as a temperature.
	std::vector<uint8_t> payload;
	payload.push_back(0x00);       // device_count = 0
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);
	payload.push_back(0x00);       // spa OFF
	payload.push_back(0x00);
	payload.push_back(0x00);
	PushTempBE(payload, 30);       // pool_target
	PushTempBE(payload, 39);       // spa_target
	PushTempBE(payload, 24);       // air
	PushTempBE(payload, 0);        // water_current = 0x0000 sentinel

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(!msg.PoolTemperature().has_value());
	BOOST_CHECK(!msg.SpaTemperature().has_value());
}

// =============================================================================
// Edge cases
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_NoPayload)
{
	auto pkt = MakePacket(0x33, 0x70, {});
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForDeviceCount)
{
	// Payload has device_count=5 but only 2 device IDs
	std::vector<uint8_t> payload = { 0x05, 0x01, 0x02 };
	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForLegacyStatusFlags)
{
	// Legacy format: has sentinel but not enough bytes for pump+spa+skip
	std::vector<uint8_t> payload;
	payload.push_back(0x01);       // device_count = 1
	payload.push_back(0x01);       // device ID
	payload.push_back(0x0E);       // sentinel
	payload.push_back(0x0F);
	payload.push_back(0x01);       // pump ON
	// Missing spa_mode and unknown bytes (need 4 total: pump+spa+unk+unk)

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForLegacyTemps)
{
	// Legacy format: has status flags but not enough bytes for 3 temperatures
	std::vector<uint8_t> payload;
	payload.push_back(0x01);
	payload.push_back(0x01);
	payload.push_back(0x0E);
	payload.push_back(0x0F);
	payload.push_back(0x01);       // pump
	payload.push_back(0x00);       // spa
	payload.push_back(0x00);       // unknown
	payload.push_back(0x00);       // unknown
	PushTempBE(payload, 280);      // pool temp
	// Missing spa and air temps (need 6 bytes total for 3 temps)

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForCurrentFormatStatusFlags)
{
	// Current format: has device IDs (no sentinel match) but not enough for 5 status bytes
	std::vector<uint8_t> payload;
	payload.push_back(0x02);       // device_count = 2
	payload.push_back(0x01);
	payload.push_back(0x02);
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);       // pool heater
	// Missing: spa, spa_heater, solar (need 5 total)

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_TooShort_ForCurrentFormatTemps)
{
	// Current format: has 5 status bytes but not enough for 4 temperatures (8 bytes)
	std::vector<uint8_t> payload;
	payload.push_back(0x02);
	payload.push_back(0x01);
	payload.push_back(0x02);
	payload.push_back(0x01);       // pump
	payload.push_back(0x00);       // pool heater
	payload.push_back(0x00);       // spa
	payload.push_back(0x00);       // spa heater
	payload.push_back(0x00);       // solar
	PushTempBE(payload, 28);       // pool_target
	// Missing: spa_target, air, water_current (need 8 bytes total for 4 temps)

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_CHECK(!msg.DeserializeContents(std::span<const uint8_t>(pkt)));
}

BOOST_AUTO_TEST_CASE(TestDeserialize_ZeroDeviceCount)
{
	// Zero devices -> goes straight to status fields (current format since no sentinel)
	std::vector<uint8_t> payload;
	payload.push_back(0x00);       // device_count = 0
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);       // pool heater = Off
	payload.push_back(0x00);       // spa OFF
	payload.push_back(0x00);       // spa heater = Off
	payload.push_back(0x00);       // solar = Off
	PushTempBE(payload, 26);       // pool_target
	PushTempBE(payload, 30);       // spa_target
	PushTempBE(payload, 20);       // air
	PushTempBE(payload, 25);       // water_current

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	BOOST_CHECK(msg.DeviceIds().empty());
	BOOST_CHECK(msg.PumpOn() == true);
	// Pool mode + pump on -> water_current (25C) is the pool's actual temperature.
	BOOST_REQUIRE(msg.PoolTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.PoolTemperature()->InCelsius().value(), 25.0));
	BOOST_REQUIRE(msg.AirTemperature().has_value());
	BOOST_CHECK(ApproxEqual(msg.AirTemperature()->InCelsius().value(), 20.0));
}

// =============================================================================
// ToString
// =============================================================================

BOOST_AUTO_TEST_CASE(TestToString_PumpOn)
{
	std::vector<uint8_t> payload;
	payload.push_back(0x00);       // device_count = 0
	payload.push_back(0x01);       // pump ON
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	PushTempBE(payload, 28);
	PushTempBE(payload, 30);
	PushTempBE(payload, 22);
	PushTempBE(payload, 27);

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	auto str = msg.ToString();
	BOOST_CHECK(str.find("Pump: ON") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestToString_PumpOff)
{
	std::vector<uint8_t> payload;
	payload.push_back(0x00);
	payload.push_back(0x00);       // pump OFF
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	PushTempBE(payload, 0);
	PushTempBE(payload, 0);
	PushTempBE(payload, 0);
	PushTempBE(payload, 0);

	auto pkt = MakePacket(0x33, 0x70, payload);
	IAQMessage_MainStatus msg;
	BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(pkt)));

	auto str = msg.ToString();
	BOOST_CHECK(str.find("Pump: OFF") != std::string::npos);
}

// =============================================================================
// SerializeContents
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSerialize_ReturnsFalse)
{
	IAQMessage_MainStatus msg;
	std::vector<uint8_t> out;
	BOOST_CHECK(!msg.SerializeContents(out));
}

BOOST_AUTO_TEST_SUITE_END()
