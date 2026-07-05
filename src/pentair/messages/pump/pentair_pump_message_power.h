#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "interfaces/imessagesignal_send.h"
#include "messages/pentair_message.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// Pump power on/off command (CMD 0x06), sent controller -> pump.
	//
	// The DATA section is a single byte: 0x0A = power on, 0x04 = power off
	// (the same run-state encoding the pump reports in its status broadcast).
	class PentairPumpMessage_Power :
		public PentairMessage,
		public Interfaces::IMessageSignalRecv<PentairPumpMessage_Power>,
		public Interfaces::IMessageSignalSend<PentairPumpMessage_Power>
	{
	public:
		static constexpr uint8_t POWER_ON = 0x0A;
		static constexpr uint8_t POWER_OFF = 0x04;

		static constexpr uint8_t Data_Index_Power = 0;

	public:
		PentairPumpMessage_Power() noexcept;
		PentairPumpMessage_Power(uint8_t from, uint8_t destination, bool power_on) noexcept;
		~PentairPumpMessage_Power() override = default;

		bool PowerOn() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		bool m_PowerOn;
	};

}
// namespace AqualinkAutomate::Pentair::Messages
