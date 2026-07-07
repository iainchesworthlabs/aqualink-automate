#include <algorithm>
#include <utility>

#include <boost/asio/write.hpp>
#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "profiling/profiling.h"
#include "serial/serial_port_config.h"
#include "serial/rfc2217/rfc2217_protocol_handler.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Serial::RFC2217
{

	ProtocolHandler::ProtocolHandler(boost::asio::ip::tcp::socket& socket) :
		ISerialPortProtocol(),
		m_Socket(socket)
	{
		LogTrace(Channel::Serial, "RFC2217 ProtocolHandler created");
	}

	void ProtocolHandler::Initialize()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::Initialize", std::source_location::current());

		LogInfo(Channel::Serial, "Starting RFC2217 protocol initialization");

		m_State = State::DATA;
		m_CommandBuffer.clear();

		InitiateNegotiation();

		LogInfo(Channel::Serial, "RFC2217 protocol initialization complete");
	}

	void ProtocolHandler::Shutdown()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::Shutdown", std::source_location::current());

		LogInfo(Channel::Serial, "Shutting down RFC2217 protocol handler");

		m_CommandBuffer.clear();
	}

	void ProtocolHandler::InitiateNegotiation()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::InitiateNegotiation", std::source_location::current());

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot initiate RFC2217 negotiation - socket not available");
			return;
		}

		LogInfo(Channel::Serial, "Initiating RFC2217 protocol negotiation");

		// Send WILL COM-PORT-OPTION
		constexpr std::array<uint8_t, 3> negotiation{ Constants::IAC, Constants::WILL, Constants::COM_PORT_OPTION };

		boost::system::error_code ec;
		boost::asio::write(m_Socket, boost::asio::buffer(negotiation), ec);

		if (ec)
		{
			LogWarning(Channel::Serial, std::format("Failed to send RFC2217 negotiation: {}", ec.message()));
			return;
		}

		LogDebug(Channel::Serial, "RFC2217 negotiation request sent");

		// Configure port with default Jandy settings
		constexpr auto default_config = Serial::SerialPortConfig::JandyDefault();
		set_baud_rate(default_config.baud_rate);
		set_character_size(default_config.character_size);
		set_parity(default_config.parity);
		set_stop_bits(default_config.stop_bits);
		set_flow_control(default_config.flow_control);
	}

	void ProtocolHandler::set_baud_rate(uint32_t baud_rate)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::set_baud_rate", std::source_location::current());

		LogDebug(Channel::Serial, std::format("RFC2217: Setting baud rate to {}", baud_rate));

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot send baud rate - socket not available");
			return;
		}

		const auto baud_bytes = EncodeBaudRate(baud_rate);
		SendCommand(Constants::SET_BAUDRATE, baud_bytes);
	}

	void ProtocolHandler::set_character_size(uint8_t size)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::set_character_size", std::source_location::current());

		LogDebug(Channel::Serial, std::format("RFC2217: Setting character size to {}", size));

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot send character size - socket not available");
			return;
		}

		SendCommand(Constants::SET_DATASIZE, std::span{ &size, 1 });
	}

	void ProtocolHandler::set_parity(Serial::Parity parity)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::set_parity", std::source_location::current());

		constexpr auto to_rfc2217_parity = [](Serial::Parity p) -> uint8_t
			{
				using enum Serial::Parity;
				switch (p)
				{
				case None:  return 1;
				case Odd:   return 2;
				case Even:  return 3;
				}
				std::unreachable();
			};

		LogDebug(Channel::Serial, std::format("RFC2217: Setting parity to {}", magic_enum::enum_name(parity)));

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot send parity - socket not available");
			return;
		}

		const uint8_t parity_value = to_rfc2217_parity(parity);
		SendCommand(Constants::SET_PARITY, std::span{ &parity_value, 1 });
	}

	void ProtocolHandler::set_stop_bits(Serial::StopBits stop_bits)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::set_stop_bits", std::source_location::current());

		constexpr auto to_rfc2217_stop = [](Serial::StopBits s) -> uint8_t
			{
				using enum Serial::StopBits;
				switch (s)
				{
				case One:           return 1;
				case OnePointFive:   return 2;
				case Two:            return 3;
				}
				std::unreachable();
			};

		LogDebug(Channel::Serial, std::format("RFC2217: Setting stop bits to {}", magic_enum::enum_name(stop_bits)));

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot send stop bits - socket not available");
			return;
		}

		const uint8_t stop_value = to_rfc2217_stop(stop_bits);
		SendCommand(Constants::SET_STOPSIZE, std::span{ &stop_value, 1 });
	}

	void ProtocolHandler::set_flow_control(Serial::FlowControl flow_control)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::set_flow_control", std::source_location::current());

		constexpr auto to_rfc2217_flow = [](Serial::FlowControl f) -> uint8_t
			{
				using enum Serial::FlowControl;
				switch (f)
				{
				case None:      return 0;
				case Software:  return 1;
				case Hardware:  return 2;
				}
				std::unreachable();
			};

		LogDebug(Channel::Serial, std::format("RFC2217: Setting flow control to {}", magic_enum::enum_name(flow_control)));

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, "Cannot send flow control - socket not available");
			return;
		}

		const uint8_t control_value = to_rfc2217_flow(flow_control);
		SendCommand(Constants::SET_CONTROL, std::span{ &control_value, 1 });
	}

	void ProtocolHandler::SendCommand(uint8_t command, std::span<const uint8_t> data)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::SendCommand", std::source_location::current());

		if (!m_Socket.is_open())
		{
			LogWarning(Channel::Serial, std::format("Cannot send RFC2217 command 0x{:02X} - socket not available", command));
			return;
		}

		LogTrace(Channel::Serial, std::format("Sending RFC2217 command 0x{:02X} with {} data bytes", command, data.size()));

		std::vector<uint8_t> packet;
		packet.reserve(6 + data.size() * 2); // Reserve space including worst-case IAC escaping

		packet.push_back(Constants::IAC);
		packet.push_back(Constants::SB);
		packet.push_back(Constants::COM_PORT_OPTION);
		packet.push_back(command);

		// Add data with IAC escaping
		for (const uint8_t byte : data)
		{
			packet.push_back(byte);
			if (byte == Constants::IAC)
			{
				packet.push_back(Constants::IAC); // Double IAC for escaping
			}
		}

		packet.push_back(Constants::IAC);
		packet.push_back(Constants::SE);

		LogTrace(Channel::Serial, std::format("RFC2217 command packet size: {} bytes", packet.size()));

		boost::system::error_code ec;
		boost::asio::write(m_Socket, boost::asio::buffer(packet), ec);

		if (ec)
		{
			LogWarning(Channel::Serial, std::format("Failed to send RFC2217 command 0x{:02X}: {}", command, ec.message()));
		}
		else
		{
			LogTrace(Channel::Serial, std::format("RFC2217 command 0x{:02X} sent successfully", command));
		}
	}

	void ProtocolHandler::HandleCommand(uint8_t command, std::span<const uint8_t> params)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("RFC2217_ProtocolHandler::HandleCommand", std::source_location::current());

		LogTrace(Channel::Serial, std::format("Handling RFC2217 command 0x{:02X} with {} parameters", command, params.size()));

		// Handle server responses (commands >= SERVER_OFFSET)
		if (command < Constants::SERVER_OFFSET)
		{
			LogTrace(Channel::Serial, std::format("RFC2217: Client command 0x{:02X} received", command));
			return;
		}

		// Security: Validate parameter lengths for known commands
		switch (const uint8_t base_command = command - Constants::SERVER_OFFSET; base_command)
		{
		case Constants::SET_BAUDRATE:
			if (params.size() != 4)
			{
				LogWarning(Channel::Serial, std::format("RFC2217: Invalid baud rate parameter length: {} (expected 4)", params.size()));
				return;
			}
			break;
		case Constants::SET_DATASIZE:
		case Constants::SET_PARITY:
		case Constants::SET_STOPSIZE:
		case Constants::SET_CONTROL:
		case Constants::NOTIFY_LINESTATE:
		case Constants::NOTIFY_MODEMSTATE:
			if (params.size() != 1)
			{
				LogWarning(Channel::Serial, std::format("RFC2217: Invalid parameter length for command 0x{:02X}: {} (expected 1)", command, params.size()));
				return;
			}
			break;
		default:
			// Unknown commands - limit parameter size as defense in depth
			if (params.size() > 256)
			{
				LogWarning(Channel::Serial, std::format("RFC2217: Excessive parameter length for unknown command 0x{:02X}: {}", command, params.size()));
				return;
			}
			break;
		}

		// Server acknowledgment - log based on specific command
		switch (command)
		{
		case Constants::SET_BAUDRATE + Constants::SERVER_OFFSET:
			LogDebug(Channel::Serial, "RFC2217: Baud rate acknowledged by server");
			break;
		case Constants::SET_DATASIZE + Constants::SERVER_OFFSET:
			LogDebug(Channel::Serial, "RFC2217: Character size acknowledged by server");
			break;
		case Constants::SET_PARITY + Constants::SERVER_OFFSET:
			LogDebug(Channel::Serial, "RFC2217: Parity acknowledged by server");
			break;
		case Constants::SET_STOPSIZE + Constants::SERVER_OFFSET:
			LogDebug(Channel::Serial, "RFC2217: Stop bits acknowledged by server");
			break;
		case Constants::SET_CONTROL + Constants::SERVER_OFFSET:
			LogDebug(Channel::Serial, "RFC2217: Flow control acknowledged by server");
			break;
		case Constants::NOTIFY_LINESTATE + Constants::SERVER_OFFSET:
			LogTrace(Channel::Serial, "RFC2217: Line state notification received");
			break;
		case Constants::NOTIFY_MODEMSTATE + Constants::SERVER_OFFSET:
			LogTrace(Channel::Serial, "RFC2217: Modem state notification received");
			break;
		default:
			LogTrace(Channel::Serial, std::format("RFC2217: Unknown server response 0x{:02X}", command));
			break;
		}
	}

	constexpr std::array<uint8_t, 4> ProtocolHandler::EncodeBaudRate(uint32_t baud_rate) noexcept
	{
		// RFC2217 uses network byte order (big-endian)
		return 
		{
			static_cast<uint8_t>((baud_rate >> 24) & 0xFF),
			static_cast<uint8_t>((baud_rate >> 16) & 0xFF),
			static_cast<uint8_t>((baud_rate >> 8) & 0xFF),
			static_cast<uint8_t>(baud_rate & 0xFF)
		};
	}

}
// namespace AqualinkAutomate::Serial::RFC2217
