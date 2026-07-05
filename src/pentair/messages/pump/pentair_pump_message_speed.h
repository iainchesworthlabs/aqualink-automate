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

	// Set-speed command (CMD 0x01), sent controller -> pump.
	//
	// The DATA section carries the target speed as a 2-byte big-endian RPM value.
	// (IntelliFlo's native register-write framing is more elaborate; this is a
	// clear, self-consistent on-wire encoding for the application's emitted
	// command, matching the decode in DeserializeContents so replay round-trips.)
	//
	// Implements both the send signal (so a device can emit it) and the receive
	// signal (so emitted/looped-back frames can be decoded and asserted in tests).
	class PentairPumpMessage_Speed :
		public PentairMessage,
		public Interfaces::IMessageSignalRecv<PentairPumpMessage_Speed>,
		public Interfaces::IMessageSignalSend<PentairPumpMessage_Speed>
	{
	public:
		static constexpr uint8_t Data_Index_RPM_High = 0;
		static constexpr uint8_t Data_Index_RPM_Low = 1;

	public:
		PentairPumpMessage_Speed() noexcept;
		PentairPumpMessage_Speed(uint8_t from, uint8_t destination, uint16_t rpm) noexcept;
		~PentairPumpMessage_Speed() override = default;

		uint16_t RPM() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint16_t m_RPM;
	};

}
// namespace AqualinkAutomate::Pentair::Messages
