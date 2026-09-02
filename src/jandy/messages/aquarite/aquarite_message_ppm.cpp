#include <bit>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/aquarite/aquarite_message_ppm.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	namespace
	{
		// Decompose a status byte that matched no named AquariteStatuses value into the
		// individual single-bit flags it is made of. Selecting candidates by popcount==1
		// (rather than a hardcoded flag list) is self-maintaining if a flag is ever added,
		// and - critically - can never select one of the 5 whole-byte sentinels (On,
		// TurningOff, Off, GeneralFault, Unknown), whose popcounts are 0, 2, 7, 7 and 8
		// respectively. Callers must only reach this once an exact enum_cast has already
		// failed, so a sentinel byte is never decomposed into a misleading flag list.
		std::vector<AquariteStatuses> DecomposeStatusFlags(uint8_t raw_status)
		{
			std::vector<AquariteStatuses> flags;

			for (const auto candidate : magic_enum::enum_values<AquariteStatuses>())
			{
				const auto bit = magic_enum::enum_integer(candidate);
				if ((std::popcount(static_cast<unsigned int>(bit)) == 1) && ((raw_status & bit) != 0))
				{
					flags.emplace_back(candidate);
				}
			}

			// Every byte reaching here is non-zero (0x00 is the exact-matched On sentinel)
			// and the 8 single-bit flags together tile every bit of a byte, so this is
			// unreachable in practice; kept as a defensive fallback rather than an assert.
			if (flags.empty())
			{
				flags.emplace_back(AquariteStatuses::Unknown);
			}

			return flags;
		}
	}
	// namespace

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

	std::vector<AquariteStatuses> AquariteMessage_PPM::StatusFlags() const
	{
		return m_StatusFlags;
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
		// carry several flags ORed together (e.g. 0x06 = LowSalt|HighSalt). A single
		// enum_cast cannot represent such a combination and falls back to Unknown for
		// m_Status (the existing single best-effort-value API, kept for compatibility);
		// StatusFlags() below additionally exposes the full decomposition so a combined
		// byte is not silently lost.
		const uint8_t raw_status = Text::ReadU8(message_bytes, Index_Status);
		if (const auto exact = magic_enum::enum_cast<AquariteStatuses>(raw_status); exact.has_value())
		{
			m_Status = *exact;
			m_StatusFlags = { m_Status };
		}
		else
		{
			m_Status = AquariteStatuses::Unknown;
			m_StatusFlags = DecomposeStatusFlags(raw_status);
		}

		return true;
	}

}
// namespace AqualinkAutomate::Messages
