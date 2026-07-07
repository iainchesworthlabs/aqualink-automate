#include <algorithm>
#include <cstddef>
#include <format>

#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	JandyMessage_MessageLong::JandyMessage_MessageLong() noexcept :
		JandyMessage_MessageLong(0, std::string())
	{
		m_Line.reserve(MAXIMUM_MESSAGE_LENGTH);
	}

	JandyMessage_MessageLong::JandyMessage_MessageLong(const uint8_t line_id_value, const std::string& line) :
		JandyMessage(JandyMessageIds::MessageLong),
		Interfaces::IMessageSignalRecv<JandyMessage_MessageLong>(),
		m_LineId(line_id_value),
		m_Line(line)
	{
		m_Line.reserve(MAXIMUM_MESSAGE_LENGTH);
	}


	uint8_t JandyMessage_MessageLong::LineId() const
	{
		return m_LineId;
	}

	std::string JandyMessage_MessageLong::Line() const
	{
		return m_Line;
	}

	std::string JandyMessage_MessageLong::ToString() const
	{
		return std::format("Packet: {} || Payload: LineId: {}, Line: '{}'", JandyMessage::ToString(), m_LineId, m_Line);
	}

	bool JandyMessage_MessageLong::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.reserve(message_bytes.size() + m_Line.size() + 1);

		message_bytes.emplace_back(static_cast<uint8_t>(m_LineId));

		for (auto& elem : m_Line)
		{
			message_bytes.emplace_back(static_cast<uint8_t>(elem));
		}

		return true;
	}

	bool JandyMessage_MessageLong::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into JandyMessage_MessageLong type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_LineId, "JandyMessage_MessageLong", "LineId"))
		{
			return false;
		}

		// At least one LineText character must be present: the payload runs from
		// Index_LineText up to the 3-byte footer, so the packet must be longer than
		// Index_LineText + PACKET_FOOTER_LENGTH.
		if (message_bytes.size() <= Index_LineText + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "JandyMessage_MessageLong is too short for content extraction");
			return false;
		}

		m_LineId = Text::ReadU8(message_bytes, Index_LineId);

		// Extract as a display line (NUL pad -> space, trailing pad stripped, leading
		// spaces preserved so the panel's centring survives), then clamp to the visible
		// display-line length.
		m_Line = Text::ExtractTrailingDisplayLine(message_bytes, Index_LineText);
		if (m_Line.size() > static_cast<std::size_t>(DISPLAY_LINE_LENGTH))
		{
			m_Line.resize(DISPLAY_LINE_LENGTH);
		}

		LogDebug(Channel::Messages, [this]() { return std::format("Deserialised JandyMessage_MessageLong: LineId -> {}, LineText -> '{}' ({} chars)", m_LineId, m_Line, m_Line.length()); });

		return true;
	}

}
// namespace AqualinkAutomate::Messages
