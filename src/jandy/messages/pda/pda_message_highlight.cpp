#include <format>

#include "messages/jandy_message_ids.h"
#include "messages/pda/pda_message_highlight.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	PDAMessage_Highlight::PDAMessage_Highlight() noexcept :
		PDAMessage_Highlight(0)
	{
	}

	PDAMessage_Highlight::PDAMessage_Highlight(const uint8_t line_id_value) :
		PDAMessage(JandyMessageIds::PDA_Highlight),
		Interfaces::IMessageSignalRecv<PDAMessage_Highlight>(),
		m_LineId(line_id_value)
	{
	}


	uint8_t PDAMessage_Highlight::LineId() const
	{
		return m_LineId;
	}

	std::string PDAMessage_Highlight::ToString() const
	{
		return std::format("Packet: {} || Payload: {}", PDAMessage::ToString(), 0);
	}

	bool PDAMessage_Highlight::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.reserve(message_bytes.size() + 3);

		message_bytes.emplace_back(static_cast<uint8_t>(m_LineId));
		message_bytes.emplace_back(0x00); // REQUIRED NUL BYTE
		message_bytes.emplace_back(0x00); // REQUIRED NUL BYTE

		return true;
	}

	bool PDAMessage_Highlight::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into PDAMessage_Highlight type", message_bytes.size()));

		if (message_bytes.size() <= Index_LineId)
		{
			LogDebug(Channel::Messages, "PDAMessage_Highlight is too short to deserialise LineId.");
		}
		else
		{
			m_LineId = static_cast<uint8_t>(message_bytes[Index_LineId]);
			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
