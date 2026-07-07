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

	enum class HeaterStates : uint8_t
	{
		Off = 0x00,
		Heating = 0x08,
		HeatPumpOff = 0x40,
		HeatPumpHeating = 0x48,
		HeatPumpCooling = 0x68,
		Unknown = 0xFF
	};

	enum class HeaterErrors : uint8_t
	{
		None = 0x00,
		SensorFault = 0x02,
		AuxMonitor = 0x08,
		HighLimit = 0x10,
		Unknown = 0xFF
	};

	class HeaterMessage_Status : public HeaterMessage, public Interfaces::IMessageSignalRecv<HeaterMessage_Status>
	{
	public:
		static const uint8_t Index_HeaterState = 4;
		static const uint8_t Index_ErrorCode = 6;

	public:
		HeaterMessage_Status() noexcept;
		~HeaterMessage_Status() override = default;
		HeaterStates HeaterState() const;
		HeaterErrors ErrorCode() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		HeaterStates m_HeaterState{ HeaterStates::Unknown };
		HeaterErrors m_ErrorCode{ HeaterErrors::Unknown };
	};

}
// namespace AqualinkAutomate::Messages
