#include <algorithm>
#include <cstddef>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_message.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	JandyMessage_Message::JandyMessage_Message() noexcept :
		JandyMessage_Message(std::string())
	{
		m_Line.reserve(MAXIMUM_MESSAGE_LENGTH);
	}

	JandyMessage_Message::JandyMessage_Message(const std::string& line) :
		JandyMessage(JandyMessageIds::Message),
		Interfaces::IMessageSignalRecv<JandyMessage_Message>(),
		m_Line(line)
	{
	}


	std::string JandyMessage_Message::Line() const
	{
		return m_Line;
	}

	std::string JandyMessage_Message::ToString() const
	{
		return std::format("Packet: {} || Payload: Line: '{}'", JandyMessage::ToString(), m_Line);
	}

	bool JandyMessage_Message::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.reserve(message_bytes.size() + m_Line.size());

		for (auto& elem : m_Line)
		{
			message_bytes.emplace_back(static_cast<uint8_t>(elem));
		}

		return true;
	}

	bool JandyMessage_Message::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into JandyMessage_Message type", message_bytes.size()); });

		// At least one LineText character must be present: the payload runs from
		// Index_LineText up to the 3-byte footer, so the packet must be longer than
		// Index_LineText + PACKET_FOOTER_LENGTH.
		if (message_bytes.size() <= Index_LineText + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "JandyMessage_Message is too short for content extraction");
			return false;
		}

		// Extract as a display line (NUL pad -> space, trailing pad stripped, leading
		// spaces preserved so the panel's centring survives), then clamp to the visible
		// display-line length.
		m_Line = Text::ExtractTrailingDisplayLine(message_bytes, Index_LineText);
		if (m_Line.size() > static_cast<std::size_t>(DISPLAY_LINE_LENGTH))
		{
			m_Line.resize(DISPLAY_LINE_LENGTH);
		}

		LogDebug(Channel::Messages, [this]() { return std::format("Deserialised JandyMessage_Message: LineText -> '{}' ({} chars)", m_Line, m_Line.length()); });

		return true;
	}

}
// namespace AqualinkAutomate::Messages
