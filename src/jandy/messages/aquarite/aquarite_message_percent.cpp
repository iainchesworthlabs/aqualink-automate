#include <format>

#include "messages/aquarite/aquarite_message_percent.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging; 

namespace AqualinkAutomate::Messages
{

	AquariteMessage_Percent::AquariteMessage_Percent() noexcept :
		AquariteMessage(JandyMessageIds::AQUARITE_Percent),
		Interfaces::IMessageSignalRecv<AquariteMessage_Percent>()
	{
	}


	uint8_t AquariteMessage_Percent::GeneratingPercentage() const
	{
		return m_Percent;
	}

	bool AquariteMessage_Percent::IsBoostMode() const
	{
		return m_Percent == Value_Boost;
	}

	bool AquariteMessage_Percent::IsServiceMode() const
	{
		return m_Percent == Value_ServiceMode;
	}

	std::string AquariteMessage_Percent::ToString() const
	{
		return std::format("Packet: {} || Payload: Percent: {}", AquariteMessage::ToString(), m_Percent);
	}

	bool AquariteMessage_Percent::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(m_Percent);

		return true;
	}

	bool AquariteMessage_Percent::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into AquariteMessage_Percent type", message_bytes.size()));

		if (message_bytes.size() <= Index_Percent)
		{
			LogDebug(Channel::Messages, "AquariteMessage_Percent is too short to deserialise Percent.");
		}
		else
		{
			m_Percent = static_cast<uint8_t>(message_bytes[Index_Percent]);
			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
