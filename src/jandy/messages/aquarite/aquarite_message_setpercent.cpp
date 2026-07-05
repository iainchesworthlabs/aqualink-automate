#include <format>

#include "messages/aquarite/aquarite_message_setpercent.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	AquariteMessage_SetPercent::AquariteMessage_SetPercent() noexcept :
		AquariteMessage(JandyMessageIds::AQUARITE_SetPercent),
		Interfaces::IMessageSignalRecv<AquariteMessage_SetPercent>()
	{
	}


	uint8_t AquariteMessage_SetPercent::GeneratingPercentage() const
	{
		return m_Percent;
	}

	bool AquariteMessage_SetPercent::IsBoostMode() const
	{
		return m_Percent == Value_Boost;
	}

	bool AquariteMessage_SetPercent::IsServiceMode() const
	{
		return m_Percent == Value_ServiceMode;
	}

	std::string AquariteMessage_SetPercent::ToString() const
	{
		return std::format("Packet: {} || Payload: Percent: {}", AquariteMessage::ToString(), m_Percent);
	}

	bool AquariteMessage_SetPercent::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(m_Percent);

		return true;
	}

	bool AquariteMessage_SetPercent::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into AquariteMessage_SetPercent type", message_bytes.size()));

		if (message_bytes.size() <= Index_Percent)
		{
			LogDebug(Channel::Messages, "AquariteMessage_SetPercent is too short to deserialise Percent.");
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
