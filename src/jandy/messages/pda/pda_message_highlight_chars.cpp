#include <format>

#include "messages/jandy_message_ids.h"
#include "messages/pda/pda_message_highlight_chars.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	PDAMessage_HighlightChars::PDAMessage_HighlightChars() noexcept :
		PDAMessage(JandyMessageIds::PDA_HighlightChars),
		Interfaces::IMessageSignalRecv<PDAMessage_HighlightChars>()
	{
	}


	uint8_t PDAMessage_HighlightChars::LineId() const
	{
		return m_LineId;
	}

	uint8_t PDAMessage_HighlightChars::StartIndex() const
	{
		return m_StartIndex;
	}

	uint8_t PDAMessage_HighlightChars::StopIndex() const
	{
		return m_StopIndex;
	}

	std::string PDAMessage_HighlightChars::ToString() const
	{
		return std::format("Packet: {} || Payload: {}", PDAMessage::ToString(), 0);
	}

	bool PDAMessage_HighlightChars::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool PDAMessage_HighlightChars::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into PDAMessage_HighlightChars type", message_bytes.size()));

		if (message_bytes.size() <= Index_LineId)
		{
			LogDebug(Channel::Messages, "PDAMessage_HighlightChars is too short to deserialise LineId.");
		}
		else if (message_bytes.size() <= Index_StartIndex)
		{
			LogDebug(Channel::Messages, "PDAMessage_HighlightChars is too short to deserialise StartIndex.");
		}
		else if (message_bytes.size() <= Index_StopIndex)
		{
			LogDebug(Channel::Messages, "PDAMessage_HighlightChars is too short to deserialise StopIndex.");
		}
		else
		{
			m_LineId = static_cast<uint8_t>(message_bytes[Index_LineId]);
			m_StartIndex = static_cast<uint8_t>(message_bytes[Index_StartIndex]);
			m_StopIndex = static_cast<uint8_t>(message_bytes[Index_StopIndex]);

			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
