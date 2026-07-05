#include <format>

#include "messages/epump/epump_message_rpm.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	EPumpMessage_RPM::EPumpMessage_RPM() noexcept :
		EPumpMessage(JandyMessageIds::EPUMP_RPM),
		Interfaces::IMessageSignalRecv<EPumpMessage_RPM>(),
		m_RPM(0)
	{
	}


	uint16_t EPumpMessage_RPM::RPM() const
	{
		return m_RPM;
	}

	std::string EPumpMessage_RPM::ToString() const
	{
		return std::format("Packet: {} || Payload: RPM: {}", EPumpMessage::ToString(), m_RPM);
	}

	bool EPumpMessage_RPM::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool EPumpMessage_RPM::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into EPumpMessage_RPM type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_RPM_High, "EPumpMessage_RPM", "RPM"))
		{
			return false;
		}

		// The raw 16-bit LE field is in quarter-RPM units on the wire.
		m_RPM = static_cast<uint16_t>(Text::ReadU16LE(message_bytes, Index_RPM_Low) / 4U);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
