#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_ids.h"
#include "messages/pentair_message_unknown.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairMessage_Unknown::PentairMessage_Unknown() noexcept :
		PentairMessage(PentairMessageIds::Unknown),
		Interfaces::IMessageSignalRecv<PentairMessage_Unknown>()
	{
	}

	const std::vector<uint8_t>& PentairMessage_Unknown::Payload() const
	{
		return m_Payload;
	}

	std::string PentairMessage_Unknown::ToString() const
	{
		return std::format("Packet: {} || Payload: {} bytes (unknown command)", PentairMessage::ToString(), m_Payload.size());
	}

	bool PentairMessage_Unknown::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(m_Payload.size()));
		message_bytes.insert(message_bytes.end(), m_Payload.begin(), m_Payload.end());
		return true;
	}

	bool PentairMessage_Unknown::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		// Retain the DATA section verbatim for diagnostics.  The data length was
		// already validated against the region size by the base class.
		const uint8_t data_length = message_bytes[Offset_Length];
		m_Payload.assign(
			message_bytes.begin() + Offset_DataStart,
			message_bytes.begin() + Offset_DataStart + data_length);

		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
