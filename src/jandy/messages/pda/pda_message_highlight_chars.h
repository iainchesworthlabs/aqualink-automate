#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pda/pda_message.h"

namespace AqualinkAutomate::Messages
{

	class PDAMessage_HighlightChars : public PDAMessage, public Interfaces::IMessageSignalRecv<PDAMessage_HighlightChars>
	{
	public:
		static const uint8_t Index_LineId = 4;
		static const uint8_t Index_StartIndex = 5;
		static const uint8_t Index_StopIndex = 6;

	public:
		PDAMessage_HighlightChars() noexcept;
		~PDAMessage_HighlightChars() override = default;
		uint8_t LineId() const;
		uint8_t StartIndex() const;
		uint8_t StopIndex() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_LineId{ 0 };
		uint8_t m_StartIndex{ 0 };
		uint8_t m_StopIndex{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
