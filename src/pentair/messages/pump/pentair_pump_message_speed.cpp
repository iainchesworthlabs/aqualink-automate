#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_ids.h"
#include "messages/pump/pentair_pump_message_speed.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairPumpMessage_Speed::PentairPumpMessage_Speed() noexcept :
		PentairMessage(PentairMessageIds::Pump_Speed),
		Interfaces::IMessageSignalRecv<PentairPumpMessage_Speed>(),
		Interfaces::IMessageSignalSend<PentairPumpMessage_Speed>()
	{
	}

	PentairPumpMessage_Speed::PentairPumpMessage_Speed(uint8_t from, uint8_t destination, uint16_t rpm) noexcept :
		PentairPumpMessage_Speed()
	{
		m_From = from;
		m_Destination = destination;
		m_RPM = rpm;
	}

	uint16_t PentairPumpMessage_Speed::RPM() const
	{
		return m_RPM;
	}

	std::string PentairPumpMessage_Speed::ToString() const
	{
		return std::format("Packet: {} || Payload: Set RPM: {}", PentairMessage::ToString(), m_RPM);
	}

	bool PentairPumpMessage_Speed::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(2)); // LEN
		message_bytes.emplace_back(static_cast<uint8_t>((m_RPM >> 8) & 0xFF));
		message_bytes.emplace_back(static_cast<uint8_t>(m_RPM & 0xFF));
		return true;
	}

	bool PentairPumpMessage_Speed::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		if (const uint8_t data_length = message_bytes[Offset_Length]; data_length <= Data_Index_RPM_Low)
		{
			LogDebug(Channel::Messages, "PentairPumpMessage_Speed DATA too short to decode RPM");
			return false;
		}

		m_RPM = static_cast<uint16_t>(
			(static_cast<uint16_t>(message_bytes[Offset_DataStart + Data_Index_RPM_High]) << 8) |
			message_bytes[Offset_DataStart + Data_Index_RPM_Low]);

		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
