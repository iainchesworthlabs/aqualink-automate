#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/heater/heater_message.h"

namespace AqualinkAutomate::Messages
{

	enum class HeaterOperatingModes : uint8_t
	{
		Off = 0x00,
		HeatPumpPoolEnabled = 0x01,
		HeatPumpSpaEnabled = 0x02,
		HeatPumpHeatingPool = 0x09,
		HeatPumpHeatingSpa = 0x0A,
		PoolEnabled = 0x11,
		SpaEnabled = 0x12,
		HeatingPool = 0x19,
		HeatingSpa = 0x1A,
		Chiller = 0x29,
		Unknown = 0xFF
	};

	class HeaterMessage_Request : public HeaterMessage, public Interfaces::IMessageSignalRecv<HeaterMessage_Request>
	{
	public:
		static const uint8_t Index_OperatingMode = 4;
		static const uint8_t Index_PoolSetpoint = 5;
		static const uint8_t Index_SpaSetpoint = 6;
		static const uint8_t Index_WaterTemperature = 7;

	public:
		HeaterMessage_Request() noexcept;
		~HeaterMessage_Request() override = default;
		HeaterOperatingModes OperatingMode() const;
		uint8_t PoolSetpoint() const;
		uint8_t SpaSetpoint() const;
		uint8_t WaterTemperature() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		HeaterOperatingModes m_OperatingMode{ HeaterOperatingModes::Unknown };
		uint8_t m_PoolSetpoint{ 0 };
		uint8_t m_SpaSetpoint{ 0 };
		uint8_t m_WaterTemperature{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
