#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pda/pda_message.h"

namespace AqualinkAutomate::Messages
{

	class PDAMessage_ShiftLines : public PDAMessage, public Interfaces::IMessageSignalRecv<PDAMessage_ShiftLines>
	{
	public:
		static const uint8_t Index_FirstLineId = 4;
		static const uint8_t Index_LastLineId = 5;
		static const uint8_t Index_LineShift = 6;

	public:
		PDAMessage_ShiftLines() noexcept;
		~PDAMessage_ShiftLines() override = default;
		uint8_t FirstLineId() const;
		uint8_t LastLineId() const;
		int8_t LineShift() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_FirstLineId{ 0 };
		uint8_t m_LastLineId{ 0 };
		int8_t m_LineShift{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
