#include <format>

#include "messages/epump/epump_message_status.h"
#include "messages/jandy_message_ids.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	EPumpMessage_Status::EPumpMessage_Status() noexcept :
		EPumpMessage(JandyMessageIds::EPUMP_Status),
		Interfaces::IMessageSignalRecv<EPumpMessage_Status>()
	{
	}


	uint8_t EPumpMessage_Status::SubCommand() const
	{
		return m_SubCommand;
	}

	std::string EPumpMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: SubCommand: 0x{:02x}", EPumpMessage::ToString(), m_SubCommand);
	}

	bool EPumpMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(m_SubCommand);

		return true;
	}

	bool EPumpMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into EPumpMessage_Status type", message_bytes.size()));

		if (message_bytes.size() <= Index_SubCommand)
		{
			LogDebug(Channel::Messages, "EPumpMessage_Status is too short to deserialise SubCommand.");
		}
		else
		{
			m_SubCommand = static_cast<uint8_t>(message_bytes[Index_SubCommand]);
			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
