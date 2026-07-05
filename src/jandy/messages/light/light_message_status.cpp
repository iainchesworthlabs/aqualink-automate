#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/light/light_message_status.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	LightMessage_Status::LightMessage_Status() noexcept :
		LightMessage(JandyMessageIds::Light_Status),
		Interfaces::IMessageSignalRecv<LightMessage_Status>()
	{
	}


	LightStates LightMessage_Status::State() const
	{
		return m_State;
	}

	bool LightMessage_Status::IsOn() const
	{
		return LightStates::On == m_State;
	}

	uint8_t LightMessage_Status::LightMode() const
	{
		return m_LightMode;
	}

	std::string LightMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: State: {}, Mode: {}", LightMessage::ToString(), magic_enum::enum_name(m_State), m_LightMode);
	}

	bool LightMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(magic_enum::enum_integer(m_State));
		message_bytes.emplace_back(m_LightMode);

		return true;
	}

	bool LightMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into LightMessage_Status type", message_bytes.size()));

		if (message_bytes.size() <= Index_State)
		{
			LogDebug(Channel::Messages, "LightMessage_Status is too short to deserialise State.");
			return false;
		}

		m_State = magic_enum::enum_cast<LightStates>(static_cast<uint8_t>(message_bytes[Index_State])).value_or(LightStates::Unknown);

		if (message_bytes.size() > Index_Mode)
		{
			m_LightMode = static_cast<uint8_t>(message_bytes[Index_Mode]);
		}

		return true;
	}

}
// namespace AqualinkAutomate::Messages
