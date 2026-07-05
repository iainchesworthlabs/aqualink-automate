#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pentair_message.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// IntelliFlo variable-speed pump status broadcast (CMD 0x07).
	//
	// The status DATA section reports the pump's live operating point.  The bytes
	// the application decodes (offsets relative to the start of DATA):
	//
	//   DATA[0]  run flag (0x0A = running, 0x04 = stopped)
	//   DATA[3]  power high byte   |
	//   DATA[4]  power low  byte   |-> watts (big-endian)
	//   DATA[5]  RPM   high byte   |
	//   DATA[6]  RPM   low  byte   |-> RPM (big-endian)
	//   DATA[7]  flow             ---> GPM (gallons per minute)
	//
	// A status frame with fewer DATA bytes than required leaves the missing
	// fields at zero.
	class PentairPumpMessage_Status : public PentairMessage, public Interfaces::IMessageSignalRecv<PentairPumpMessage_Status>
	{
	public:
		// DATA-relative offsets.
		static constexpr uint8_t Data_Index_RunState = 0;
		static constexpr uint8_t Data_Index_Watts_High = 3;
		static constexpr uint8_t Data_Index_Watts_Low = 4;
		static constexpr uint8_t Data_Index_RPM_High = 5;
		static constexpr uint8_t Data_Index_RPM_Low = 6;
		static constexpr uint8_t Data_Index_GPM = 7;

		static constexpr uint8_t RUN_STATE_RUNNING = 0x0A;

	public:
		PentairPumpMessage_Status() noexcept;
		~PentairPumpMessage_Status() override = default;

		uint16_t RPM() const;
		uint16_t Watts() const;
		uint8_t GPM() const;
		bool IsRunning() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint16_t m_RPM{0};
		uint16_t m_Watts{0};
		uint8_t m_GPM{0};
		bool m_IsRunning{false};
	};

}
// namespace AqualinkAutomate::Pentair::Messages
