#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "interfaces/iserialportprotocol.h"
#include "serial/serial_port_config.h"
#include "serial/rfc2217/rfc2217_constants.h"
#include "serial/rfc2217/rfc2217_types.h"

namespace AqualinkAutomate::Serial::RFC2217
{
	class ProtocolHandler : public Interfaces::ISerialPortProtocol
	{
	public:
		explicit ProtocolHandler(boost::asio::ip::tcp::socket& socket);
		~ProtocolHandler() override = default;

		void Initialize() override;
		void Shutdown() override;

		void set_baud_rate(uint32_t baud_rate) override;
		void set_character_size(uint8_t size) override;
		void set_parity(Serial::Parity parity) override;
		void set_stop_bits(Serial::StopBits stop_bits) override;
		void set_flow_control(Serial::FlowControl flow_control) override;

		// Strip RFC2217/telnet IAC control sequences from a raw inbound socket
		// buffer, compacting the surviving data bytes to the front of the same
		// buffer (in place). Returns the number of data bytes written.  IAC
		// command/subnegotiation bytes are consumed by the state machine and do
		// NOT appear in the output, so they never leak into the Jandy stream.
		[[nodiscard]] std::size_t FilterInboundData(std::span<uint8_t> raw);

		// Single-byte filter step.  Returns the decoded data byte, or nullopt when
		// the byte was consumed as part of an IAC control sequence.
		[[nodiscard]] std::optional<uint8_t> ProcessByte(uint8_t byte);

	private:
		void InitiateNegotiation();
		void SendCommand(uint8_t command, std::span<const uint8_t> data = {});
		void HandleCommand(uint8_t command, std::span<const uint8_t> params);

		[[nodiscard]] static constexpr std::array<uint8_t, 4> EncodeBaudRate(uint32_t baud_rate) noexcept;

		[[nodiscard]] std::optional<uint8_t> ProcessCommand(uint8_t byte);
		[[nodiscard]] std::optional<uint8_t> ProcessIAC(uint8_t byte);
		[[nodiscard]] std::optional<uint8_t> ProcessSubnegIAC(uint8_t byte);

	private:
		boost::asio::ip::tcp::socket& m_Socket;

		RFC2217::State m_State{ State::DATA };
		std::vector<uint8_t> m_CommandBuffer;

		// Security: Maximum command buffer size to prevent memory exhaustion from malicious input
		static constexpr std::size_t MAX_COMMAND_BUFFER_SIZE = 4096;
	};

}
// namespace AqualinkAutomate::Serial::RFC2217
