#include <format>

#include "messages/iaq/iaq_message_device_id.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	IAQMessage_DeviceId::IAQMessage_DeviceId() noexcept :
		IAQMessage(JandyMessageIds::IAQ_DeviceId),
		Interfaces::IMessageSignalRecv<IAQMessage_DeviceId>()
	{
	}

	const std::string& IAQMessage_DeviceId::DeviceId() const
	{
		return m_DeviceId;
	}

	std::string IAQMessage_DeviceId::ToString() const
	{
		return std::format("Packet: {} || DeviceId: '{}'", IAQMessage::ToString(), m_DeviceId);
	}

	bool IAQMessage_DeviceId::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		// Receive-only: the master sends this to the device; we never originate it.
		return false;
	}

	bool IAQMessage_DeviceId::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into IAQMessage_DeviceId type", message_bytes.size()); });

		m_DeviceId.clear();

		// message_bytes is the whole framed packet. The ASCII id payload runs from
		// Index_DataStart (4) up to (but not including) the 3-byte footer (checksum + DLE +
		// ETX), and is itself NUL-terminated.
		if (message_bytes.size() <= (Index_DataStart + PACKET_FOOTER_LENGTH))
		{
			LogDebug(Channel::Messages, "IAQMessage_DeviceId: packet too short to contain an id payload");
			return false;
		}

		const std::size_t payload_end = message_bytes.size() - PACKET_FOOTER_LENGTH;
		for (std::size_t i = Index_DataStart; i < payload_end; ++i)
		{
			const uint8_t byte = Text::ReadU8(message_bytes, i);
			if (byte == 0x00)
			{
				break;  // NUL terminator
			}
			// Sanitise wire-sourced id text to printable ASCII before it is surfaced
			// to logs / UI (the bytes are otherwise an arbitrary device-controlled string).
			m_DeviceId.push_back(Text::SanitisePrintableAsciiByte(byte));
		}

		return true;
	}

}
// namespace AqualinkAutomate::Messages
