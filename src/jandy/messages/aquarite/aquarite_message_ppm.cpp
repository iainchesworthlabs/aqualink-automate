#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/aquarite/aquarite_message_ppm.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	AquariteMessage_PPM::AquariteMessage_PPM() noexcept :
		AquariteMessage(JandyMessageIds::AQUARITE_PPM),
		Interfaces::IMessageSignalRecv<AquariteMessage_PPM>()
	{
	}


	uint16_t AquariteMessage_PPM::SaltConcentrationPPM() const
	{
		return m_PPM;
	}

	AquariteStatuses AquariteMessage_PPM::Status() const
	{
		return m_Status;
	}

	std::string AquariteMessage_PPM::ToString() const
	{
		return std::format("Packet: {} || Payload: PPM: {}, Status: {}", AquariteMessage::ToString(), m_PPM, magic_enum::enum_name(m_Status));
	}

	bool AquariteMessage_PPM::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(m_PPM / 100));
		message_bytes.emplace_back(magic_enum::enum_integer(m_Status));

		return true;
	}

	bool AquariteMessage_PPM::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into AquariteMessage_PPM type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_PPM, "AquariteMessage_PPM", "PPM"))
		{
			return false;
		}

		if (!Text::RequireIndex(message_bytes, Index_Status, "AquariteMessage_PPM", "Status"))
		{
			return false;
		}

		m_PPM = static_cast<uint16_t>(Text::ReadU8(message_bytes, Index_PPM) * 100);

		// NOTE: AquariteStatuses is partly a bit-flag set (Warning_NoFlow=0x01,
		// Warning_LowSalt=0x02, Warning_HighSalt=0x04, ...), so a received byte can
		// carry several flags ORed together (e.g. 0x06 = LowSalt|HighSalt).  A single
		// enum_cast cannot represent such a combination and falls back to Unknown.
		// The bit-by-bit warning interpretation belongs to the consuming device
		// (AquariteDevice), which has the kernel health model to express it; here we
		// keep the single best-effort enumerator for the existing status API.
		m_Status = magic_enum::enum_cast<AquariteStatuses>(Text::ReadU8(message_bytes, Index_Status)).value_or(AquariteStatuses::Unknown);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
