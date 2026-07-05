#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_ids.h"
#include "messages/pump/pentair_pump_message_power.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairPumpMessage_Power::PentairPumpMessage_Power() noexcept :
		PentairMessage(PentairMessageIds::Pump_Power),
		Interfaces::IMessageSignalRecv<PentairPumpMessage_Power>(),
		Interfaces::IMessageSignalSend<PentairPumpMessage_Power>()
	{
	}

	PentairPumpMessage_Power::PentairPumpMessage_Power(uint8_t from, uint8_t destination, bool power_on) noexcept :
		PentairPumpMessage_Power()
	{
		m_From = from;
		m_Destination = destination;
		m_PowerOn = power_on;
	}

	bool PentairPumpMessage_Power::PowerOn() const
	{
		return m_PowerOn;
	}

	std::string PentairPumpMessage_Power::ToString() const
	{
		return std::format("Packet: {} || Payload: Power: {}", PentairMessage::ToString(), m_PowerOn ? "ON" : "OFF");
	}

	bool PentairPumpMessage_Power::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(static_cast<uint8_t>(1)); // LEN
		message_bytes.emplace_back(m_PowerOn ? POWER_ON : POWER_OFF);
		return true;
	}

	bool PentairPumpMessage_Power::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		if (const uint8_t data_length = message_bytes[Offset_Length]; data_length <= Data_Index_Power)
		{
			LogDebug(Channel::Messages, "PentairPumpMessage_Power DATA too short to decode power state");
			return false;
		}

		m_PowerOn = (message_bytes[Offset_DataStart + Data_Index_Power] == POWER_ON);
		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
