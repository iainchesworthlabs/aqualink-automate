#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pentair_message.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// IntelliChlor salt-water generator status (CMD 0x12), broadcast by the
	// chlorinator.  DATA layout (offsets relative to the start of DATA):
	//
	//   DATA[0] salt level, raw   ---> salt PPM = raw * 50
	//   DATA[1] output percentage ---> 0 - 100 %
	//   DATA[2] status flags      ---> bitfield (bit0 low-salt, bit1 high-salt,
	//                                  bit2 low-flow, bit4 clean-cell)
	//
	// A status frame with fewer DATA bytes leaves the missing fields at zero.
	class PentairChlorinatorMessage_Status : public PentairMessage, public Interfaces::IMessageSignalRecv<PentairChlorinatorMessage_Status>
	{
	public:
		static constexpr uint8_t Data_Index_Salt = 0;
		static constexpr uint8_t Data_Index_Output = 1;
		static constexpr uint8_t Data_Index_StatusFlags = 2;

		static constexpr uint16_t SALT_PPM_PER_UNIT = 50;

		// Status-flag bit masks.
		static constexpr uint8_t FLAG_LOW_SALT = 0x01;
		static constexpr uint8_t FLAG_HIGH_SALT = 0x02;
		static constexpr uint8_t FLAG_LOW_FLOW = 0x04;
		static constexpr uint8_t FLAG_CLEAN_CELL = 0x10;

	public:
		PentairChlorinatorMessage_Status() noexcept;
		~PentairChlorinatorMessage_Status() override = default;

		uint16_t SaltPPM() const;
		uint8_t OutputPercent() const;
		uint8_t StatusFlags() const;

		bool IsLowSalt() const;
		bool IsHighSalt() const;
		bool IsLowFlow() const;
		bool NeedsCleanCell() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint16_t m_SaltPPM{0};
		uint8_t m_OutputPercent{0};
		uint8_t m_StatusFlags{0};
	};

}
// namespace AqualinkAutomate::Pentair::Messages
