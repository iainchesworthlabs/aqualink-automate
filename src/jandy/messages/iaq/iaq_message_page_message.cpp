#include <cstddef>
#include <format>

#include "messages/iaq/iaq_message_page_message.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	IAQMessage_PageMessage::IAQMessage_PageMessage() noexcept :
		IAQMessage(JandyMessageIds::IAQ_PageMessage),
		Interfaces::IMessageSignalRecv<IAQMessage_PageMessage>()
	{
	}


	uint8_t IAQMessage_PageMessage::LineId() const
	{
		return m_LineId;
	}

	std::string IAQMessage_PageMessage::Line() const
	{
		return m_Line;
	}

	std::string IAQMessage_PageMessage::ToString() const
	{
		return std::format("Packet: {} || Payload: LineId: {}, Line: '{}'", IAQMessage::ToString(), m_LineId, m_Line);
	}

	bool IAQMessage_PageMessage::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool IAQMessage_PageMessage::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into IAQMessage_PageMessage type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_LineId, "IAQMessage_PageMessage", "LineId"))
		{
			return false;
		}

		// At least one LineText character must be present: the payload runs from
		// Index_LineText up to the 3-byte footer, so the packet must be longer than
		// Index_LineText + PACKET_FOOTER_LENGTH.
		if (message_bytes.size() <= Index_LineText + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "IAQMessage_PageMessage is too short for content extraction");
			return false;
		}

		m_LineId = Text::ReadU8(message_bytes, Index_LineId);
		// Display line: NUL pad -> space / trailing pad stripped, leading spaces kept.
		m_Line = Text::ExtractTrailingDisplayLine(message_bytes, Index_LineText);

		LogDebug(Channel::Messages, [this]() { return std::format("Deserialised IAQMessage_PageMessage: LineId -> {}, LineText -> '{}' ({} chars)", m_LineId, m_Line, m_Line.length()); });

		return true;
	}

}
// namespace AqualinkAutomate::Messages
