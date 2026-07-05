#include <array>
#include <format>

#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_decode_helpers.h"
#include "messages/pentair_message_ids.h"
#include "messages/controller/pentair_controller_message_status.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Messages
{

	PentairControllerMessage_Status::PentairControllerMessage_Status() noexcept :
		PentairMessage(PentairMessageIds::Controller_Status),
		Interfaces::IMessageSignalRecv<PentairControllerMessage_Status>()
	{
	}

	uint8_t PentairControllerMessage_Status::Hour() const { return m_Hour; }
	uint8_t PentairControllerMessage_Status::Minute() const { return m_Minute; }

	bool PentairControllerMessage_Status::SpaCircuitOn() const { return (m_CircuitMask & CIRCUIT_SPA) != 0; }
	bool PentairControllerMessage_Status::PoolCircuitOn() const { return (m_CircuitMask & CIRCUIT_POOL) != 0; }

	bool PentairControllerMessage_Status::HasWaterTemp() const { return m_HasWaterTemp; }
	uint8_t PentairControllerMessage_Status::WaterTempF() const { return m_WaterTempF; }

	bool PentairControllerMessage_Status::HasAirTemp() const { return m_HasAirTemp; }
	uint8_t PentairControllerMessage_Status::AirTempF() const { return m_AirTempF; }

	bool PentairControllerMessage_Status::PoolHeaterOn() const { return (m_HeaterMask & HEATER_POOL) != 0; }
	bool PentairControllerMessage_Status::SpaHeaterOn() const { return (m_HeaterMask & HEATER_SPA) != 0; }

	std::string PentairControllerMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: {:02}:{:02}, circuits 0x{:02x}, water {}F, air {}F, heater 0x{:02x}",
			PentairMessage::ToString(), m_Hour, m_Minute, m_CircuitMask, m_WaterTempF, m_AirTempF, m_HeaterMask);
	}

	bool PentairControllerMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		std::array<uint8_t, 6> data{};
		data[Data_Index_Hour] = m_Hour;
		data[Data_Index_Minute] = m_Minute;
		data[Data_Index_CircuitMask] = m_CircuitMask;
		data[Data_Index_WaterTempF] = m_WaterTempF;
		data[Data_Index_AirTempF] = m_AirTempF;
		data[Data_Index_HeaterMask] = m_HeaterMask;

		message_bytes.emplace_back(static_cast<uint8_t>(data.size())); // LEN
		message_bytes.insert(message_bytes.end(), data.begin(), data.end());
		return true;
	}

	bool PentairControllerMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		const uint8_t data_length = DataLengthOf(message_bytes);

		auto data_at = [&](uint8_t data_index)
		{
			return DataByteAt(message_bytes, data_index);
		};

		m_Hour = data_at(Data_Index_Hour);
		m_Minute = data_at(Data_Index_Minute);
		m_CircuitMask = data_at(Data_Index_CircuitMask);

		m_HasWaterTemp = (Data_Index_WaterTempF < data_length);
		m_WaterTempF = data_at(Data_Index_WaterTempF);

		m_HasAirTemp = (Data_Index_AirTempF < data_length);
		m_AirTempF = data_at(Data_Index_AirTempF);

		m_HeaterMask = data_at(Data_Index_HeaterMask);

		return true;
	}

}
// namespace AqualinkAutomate::Pentair::Messages
