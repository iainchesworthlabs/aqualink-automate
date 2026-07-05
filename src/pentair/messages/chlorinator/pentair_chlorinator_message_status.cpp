#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_decode_helpers.h"
#include "messages/pentair_message_ids.h"
#include "messages/chlorinator/pentair_chlorinator_message_status.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairChlorinatorMessage_Status::PentairChlorinatorMessage_Status() noexcept :
		PentairMessage(PentairMessageIds::Chlorinator_Status),
		Interfaces::IMessageSignalRecv<PentairChlorinatorMessage_Status>()
	{
	}

	uint16_t PentairChlorinatorMessage_Status::SaltPPM() const
	{
		return m_SaltPPM;
	}

	uint8_t PentairChlorinatorMessage_Status::OutputPercent() const
	{
		return m_OutputPercent;
	}

	uint8_t PentairChlorinatorMessage_Status::StatusFlags() const
	{
		return m_StatusFlags;
	}

	bool PentairChlorinatorMessage_Status::IsLowSalt() const
	{
		return (m_StatusFlags & FLAG_LOW_SALT) != 0;
	}

	bool PentairChlorinatorMessage_Status::IsHighSalt() const
	{
		return (m_StatusFlags & FLAG_HIGH_SALT) != 0;
	}

	bool PentairChlorinatorMessage_Status::IsLowFlow() const
	{
		return (m_StatusFlags & FLAG_LOW_FLOW) != 0;
	}

	bool PentairChlorinatorMessage_Status::NeedsCleanCell() const
	{
		return (m_StatusFlags & FLAG_CLEAN_CELL) != 0;
	}

	std::string PentairChlorinatorMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: Salt: {} PPM, Output: {}%, Flags: 0x{:02x}",
			PentairMessage::ToString(), m_SaltPPM, m_OutputPercent, m_StatusFlags);
	}

	bool PentairChlorinatorMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(3)); // LEN
		message_bytes.emplace_back(static_cast<uint8_t>(m_SaltPPM / SALT_PPM_PER_UNIT));
		message_bytes.emplace_back(m_OutputPercent);
		message_bytes.emplace_back(m_StatusFlags);
		return true;
	}

	bool PentairChlorinatorMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		const uint8_t data_length = DataLengthOf(message_bytes);

		auto data_at = [&](uint8_t data_index) -> uint8_t
		{
			return DataByteAt(message_bytes, data_index);
		};

		if (data_length <= Data_Index_StatusFlags)
		{
			LogDebug(Channel::Messages, [data_length] { return std::format("PentairChlorinatorMessage_Status DATA too short ({} bytes); missing fields default to zero", data_length); });
		}

		m_SaltPPM = static_cast<uint16_t>(data_at(Data_Index_Salt)) * SALT_PPM_PER_UNIT;
		m_OutputPercent = data_at(Data_Index_Output);
		m_StatusFlags = data_at(Data_Index_StatusFlags);

		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
