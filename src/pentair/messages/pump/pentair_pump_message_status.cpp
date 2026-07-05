#include <array>
#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_decode_helpers.h"
#include "messages/pentair_message_ids.h"
#include "messages/pump/pentair_pump_message_status.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairPumpMessage_Status::PentairPumpMessage_Status() noexcept :
		PentairMessage(PentairMessageIds::Pump_Status),
		Interfaces::IMessageSignalRecv<PentairPumpMessage_Status>()
	{
	}

	uint16_t PentairPumpMessage_Status::RPM() const
	{
		return m_RPM;
	}

	uint16_t PentairPumpMessage_Status::Watts() const
	{
		return m_Watts;
	}

	uint8_t PentairPumpMessage_Status::GPM() const
	{
		return m_GPM;
	}

	bool PentairPumpMessage_Status::IsRunning() const
	{
		return m_IsRunning;
	}

	std::string PentairPumpMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: RPM: {}, Watts: {}, GPM: {}, Running: {}",
			PentairMessage::ToString(), m_RPM, m_Watts, m_GPM, m_IsRunning);
	}

	bool PentairPumpMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		// Emit an 8-byte DATA section consistent with the decode offsets above.
		std::array<uint8_t, 8> data{};
		data[Data_Index_RunState] = m_IsRunning ? RUN_STATE_RUNNING : 0x04;
		data[Data_Index_Watts_High] = static_cast<uint8_t>((m_Watts >> 8) & 0xFF);
		data[Data_Index_Watts_Low] = static_cast<uint8_t>(m_Watts & 0xFF);
		data[Data_Index_RPM_High] = static_cast<uint8_t>((m_RPM >> 8) & 0xFF);
		data[Data_Index_RPM_Low] = static_cast<uint8_t>(m_RPM & 0xFF);
		data[Data_Index_GPM] = m_GPM;

		message_bytes.emplace_back(static_cast<uint8_t>(data.size())); // LEN
		message_bytes.insert(message_bytes.end(), data.begin(), data.end());
		return true;
	}

	bool PentairPumpMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		const uint8_t data_length = DataLengthOf(message_bytes);

		auto data_at = [&](uint8_t data_index)
		{
			return DataByteAt(message_bytes, data_index);
		};

		if (data_length <= Data_Index_GPM)
		{
			LogDebug(Channel::Messages, [data_length] { return std::format("PentairPumpMessage_Status DATA too short ({} bytes) to decode all fields; missing fields default to zero", data_length); });
		}

		m_Watts = static_cast<uint16_t>((static_cast<uint16_t>(data_at(Data_Index_Watts_High)) << 8) | data_at(Data_Index_Watts_Low));
		m_RPM = static_cast<uint16_t>((static_cast<uint16_t>(data_at(Data_Index_RPM_High)) << 8) | data_at(Data_Index_RPM_Low));
		m_GPM = data_at(Data_Index_GPM);
		m_IsRunning = (data_at(Data_Index_RunState) == RUN_STATE_RUNNING);

		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
