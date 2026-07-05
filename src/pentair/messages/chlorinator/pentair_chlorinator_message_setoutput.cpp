#include <algorithm>
#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_ids.h"
#include "messages/chlorinator/pentair_chlorinator_message_setoutput.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairChlorinatorMessage_SetOutput::PentairChlorinatorMessage_SetOutput() noexcept :
		PentairMessage(PentairMessageIds::Chlorinator_SetOutput),
		Interfaces::IMessageSignalRecv<PentairChlorinatorMessage_SetOutput>(),
		Interfaces::IMessageSignalSend<PentairChlorinatorMessage_SetOutput>(),
		m_OutputPercent(0)
	{
	}

	PentairChlorinatorMessage_SetOutput::PentairChlorinatorMessage_SetOutput(uint8_t from, uint8_t destination, uint8_t output_percent) noexcept :
		PentairChlorinatorMessage_SetOutput()
	{
		m_From = from;
		m_Destination = destination;
		m_OutputPercent = std::min<uint8_t>(output_percent, 100);
	}

	uint8_t PentairChlorinatorMessage_SetOutput::OutputPercent() const
	{
		return m_OutputPercent;
	}

	std::string PentairChlorinatorMessage_SetOutput::ToString() const
	{
		return std::format("Packet: {} || Payload: Set output: {}%", PentairMessage::ToString(), m_OutputPercent);
	}

	bool PentairChlorinatorMessage_SetOutput::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(1)); // LEN
		message_bytes.emplace_back(m_OutputPercent);
		return true;
	}

	bool PentairChlorinatorMessage_SetOutput::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		if (const uint8_t data_length = message_bytes[Offset_Length]; data_length <= Data_Index_Output)
		{
			LogDebug(Channel::Messages, "PentairChlorinatorMessage_SetOutput DATA too short to decode output");
			return false;
		}

		m_OutputPercent = message_bytes[Offset_DataStart + Data_Index_Output];
		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
