#include <format>

#include "messages/epump/epump_message_watts.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	EPumpMessage_Watts::EPumpMessage_Watts() noexcept :
		EPumpMessage(JandyMessageIds::EPUMP_Watts),
		Interfaces::IMessageSignalRecv<EPumpMessage_Watts>()
	{
	}


	uint16_t EPumpMessage_Watts::Watts() const
	{
		return m_Watts;
	}

	std::string EPumpMessage_Watts::ToString() const
	{
		return std::format("Packet: {} || Payload: Watts: {}", EPumpMessage::ToString(), m_Watts);
	}

	bool EPumpMessage_Watts::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool EPumpMessage_Watts::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into EPumpMessage_Watts type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_Watts_High, "EPumpMessage_Watts", "Watts"))
		{
			return false;
		}

		m_Watts = Text::ReadU16LE(message_bytes, Index_Watts_Low);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
