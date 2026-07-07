#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/chemlink/chemlink_message_response.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	ChemlinkMessage_Response::ChemlinkMessage_Response() noexcept :
		ChemlinkMessage(JandyMessageIds::Chemlink_Response),
		Interfaces::IMessageSignalRecv<ChemlinkMessage_Response>()
	{
	}


	ChemlinkDataTag ChemlinkMessage_Response::DataTag() const
	{
		return m_DataTag;
	}

	uint16_t ChemlinkMessage_Response::RawValue() const
	{
		return m_RawValue;
	}

	std::string ChemlinkMessage_Response::ToString() const
	{
		return std::format("Packet: {} || Payload: DataTag: {}, RawValue: {}", ChemlinkMessage::ToString(), magic_enum::enum_name(m_DataTag), m_RawValue);
	}

	bool ChemlinkMessage_Response::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool ChemlinkMessage_Response::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into ChemlinkMessage_Response type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_DataTag, "ChemlinkMessage_Response", "DataTag"))
		{
			return false;
		}

		m_DataTag = magic_enum::enum_cast<ChemlinkDataTag>(Text::ReadU8(message_bytes, Index_DataTag)).value_or(ChemlinkDataTag::Unknown);

		if (message_bytes.size() > Index_ValueHigh)
		{
			m_RawValue = Text::ReadU16LE(message_bytes, Index_ValueLow);
		}
		else if (message_bytes.size() > Index_ValueLow)
		{
			m_RawValue = Text::ReadU8(message_bytes, Index_ValueLow);
		}

		return true;
	}

}
// namespace AqualinkAutomate::Messages
