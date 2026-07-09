#include <cstddef>
#include <format>

#include "messages/iaq/iaq_message_table_message.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	IAQMessage_TableMessage::IAQMessage_TableMessage() noexcept :
		IAQMessage(JandyMessageIds::IAQ_TableMessage),
		Interfaces::IMessageSignalRecv<IAQMessage_TableMessage>()
	{
	}


	uint8_t IAQMessage_TableMessage::LineId() const
	{
		return m_LineId;
	}

	uint8_t IAQMessage_TableMessage::Attribute() const
	{
		return m_Attribute;
	}

	std::string IAQMessage_TableMessage::Line() const
	{
		return m_Line;
	}

	std::string IAQMessage_TableMessage::ToString() const
	{
		return std::format("Packet: {} || Payload: LineId: {}, Attribute: {}, Line: '{}'", IAQMessage::ToString(), m_LineId, m_Attribute, m_Line);
	}

	bool IAQMessage_TableMessage::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool IAQMessage_TableMessage::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into IAQMessage_TableMessage type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_LineId, "IAQMessage_TableMessage", "LineId"))
		{
			return false;
		}

		// At least one LineText character must be present: the payload runs from
		// Index_LineText up to the 3-byte footer, so the packet must be longer than
		// Index_LineText + PACKET_FOOTER_LENGTH.
		if (message_bytes.size() <= Index_LineText + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "IAQMessage_TableMessage is too short for content extraction");
			return false;
		}

		m_LineId = Text::ReadU8(message_bytes, Index_LineId);
		m_Attribute = Text::ReadU8(message_bytes, Index_Attribute);
		// Display line: NUL pad -> space / trailing pad stripped, leading spaces kept.
		m_Line = Text::ExtractTrailingDisplayLine(message_bytes, Index_LineText);

		LogDebug(Channel::Messages, [this]() { return std::format("Deserialised IAQMessage_TableMessage: LineId -> {}, Attribute -> {}, LineText -> '{}' ({} chars)", m_LineId, m_Attribute, m_Line, m_Line.length()); });

		return true;
	}

}
// namespace AqualinkAutomate::Messages
