#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/heater/heater_message_status.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	HeaterMessage_Status::HeaterMessage_Status() noexcept :
		HeaterMessage(JandyMessageIds::Heater_Status),
		Interfaces::IMessageSignalRecv<HeaterMessage_Status>()
	{
	}


	HeaterStates HeaterMessage_Status::HeaterState() const
	{
		return m_HeaterState;
	}

	HeaterErrors HeaterMessage_Status::ErrorCode() const
	{
		return m_ErrorCode;
	}

	std::string HeaterMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: State: {}, Error: {}", HeaterMessage::ToString(), magic_enum::enum_name(m_HeaterState), magic_enum::enum_name(m_ErrorCode));
	}

	bool HeaterMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(magic_enum::enum_integer(m_HeaterState));
		message_bytes.emplace_back(0x00); // Byte [5] is unused/reserved
		message_bytes.emplace_back(magic_enum::enum_integer(m_ErrorCode));

		return true;
	}

	bool HeaterMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into HeaterMessage_Status type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_HeaterState, "HeaterMessage_Status", "HeaterState"))
		{
			return false;
		}

		if (!Text::RequireIndex(message_bytes, Index_ErrorCode, "HeaterMessage_Status", "ErrorCode"))
		{
			return false;
		}

		m_HeaterState = magic_enum::enum_cast<HeaterStates>(Text::ReadU8(message_bytes, Index_HeaterState)).value_or(HeaterStates::Unknown);
		m_ErrorCode = magic_enum::enum_cast<HeaterErrors>(Text::ReadU8(message_bytes, Index_ErrorCode)).value_or(HeaterErrors::Unknown);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
