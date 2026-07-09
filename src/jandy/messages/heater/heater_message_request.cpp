#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/heater/heater_message_request.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	HeaterMessage_Request::HeaterMessage_Request() noexcept :
		HeaterMessage(JandyMessageIds::Heater_Request),
		Interfaces::IMessageSignalRecv<HeaterMessage_Request>()
	{
	}


	HeaterOperatingModes HeaterMessage_Request::OperatingMode() const
	{
		return m_OperatingMode;
	}

	uint8_t HeaterMessage_Request::PoolSetpoint() const
	{
		return m_PoolSetpoint;
	}

	uint8_t HeaterMessage_Request::SpaSetpoint() const
	{
		return m_SpaSetpoint;
	}

	uint8_t HeaterMessage_Request::WaterTemperature() const
	{
		return m_WaterTemperature;
	}

	std::string HeaterMessage_Request::ToString() const
	{
		return std::format("Packet: {} || Payload: Mode: {}, Pool Setpoint: {}, Spa Setpoint: {}, Water Temp: {}", HeaterMessage::ToString(), magic_enum::enum_name(m_OperatingMode), m_PoolSetpoint, m_SpaSetpoint, m_WaterTemperature);
	}

	bool HeaterMessage_Request::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(magic_enum::enum_integer(m_OperatingMode));
		message_bytes.emplace_back(m_PoolSetpoint);
		message_bytes.emplace_back(m_SpaSetpoint);
		message_bytes.emplace_back(m_WaterTemperature);

		return true;
	}

	bool HeaterMessage_Request::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into HeaterMessage_Request type", message_bytes.size()));

		if (message_bytes.size() <= Index_OperatingMode)
		{
			LogDebug(Channel::Messages, "HeaterMessage_Request is too short to deserialise OperatingMode.");
		}
		else if (message_bytes.size() <= Index_PoolSetpoint)
		{
			LogDebug(Channel::Messages, "HeaterMessage_Request is too short to deserialise PoolSetpoint.");
		}
		else if (message_bytes.size() <= Index_SpaSetpoint)
		{
			LogDebug(Channel::Messages, "HeaterMessage_Request is too short to deserialise SpaSetpoint.");
		}
		else if (message_bytes.size() <= Index_WaterTemperature)
		{
			LogDebug(Channel::Messages, "HeaterMessage_Request is too short to deserialise WaterTemperature.");
		}
		else
		{
			m_OperatingMode = magic_enum::enum_cast<HeaterOperatingModes>(static_cast<uint8_t>(message_bytes[Index_OperatingMode])).value_or(HeaterOperatingModes::Unknown);
			m_PoolSetpoint = static_cast<uint8_t>(message_bytes[Index_PoolSetpoint]);
			m_SpaSetpoint = static_cast<uint8_t>(message_bytes[Index_SpaSetpoint]);
			m_WaterTemperature = static_cast<uint8_t>(message_bytes[Index_WaterTemperature]);

			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
