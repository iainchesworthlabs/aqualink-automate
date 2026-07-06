#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include "developer/serial_port_options.h"
#include "interfaces/iserialportimpl.h"
#include "interfaces/iserialportprotocol.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "serial/rfc2217/rfc2217_types.h"

namespace AqualinkAutomate::Serial::PortTypes
{

	class NetworkSerialPortImpl : public Interfaces::ISerialPortImpl
	{
	public:
		// use_rfc2217: when true (the default) the remote stream is treated as an
		// RFC2217 telnet transport — the handler negotiates com-port options and
		// strips inbound IAC sequences. When false (raw TCP: --rawtcp / --no-rfc2217)
		// no telnet handler is installed, so no negotiation is sent and inbound bytes
		// (including 0xFF, a legal serial value) pass through unfiltered.
		explicit NetworkSerialPortImpl(boost::asio::any_io_executor executor, bool use_rfc2217 = true);
		NetworkSerialPortImpl(boost::asio::any_io_executor executor, const std::string& endpoint_name, bool use_rfc2217 = true);
		NetworkSerialPortImpl(boost::asio::any_io_executor executor, const std::string& endpoint_name, boost::system::error_code& ec, bool use_rfc2217 = true);
		~NetworkSerialPortImpl() override;

		NetworkSerialPortImpl(const NetworkSerialPortImpl&) = delete;
		NetworkSerialPortImpl& operator=(const NetworkSerialPortImpl&) = delete;
		NetworkSerialPortImpl(NetworkSerialPortImpl&&) noexcept = delete;
		NetworkSerialPortImpl& operator=(NetworkSerialPortImpl&&) noexcept = delete;

		void open(const std::string& endpoint_name) override;
		void open(const std::string& endpoint_name, boost::system::error_code& ec) override;
		bool is_open() const override;
		void close() override;
		void close(boost::system::error_code& ec) override;

		void cancel() override;
		void cancel(boost::system::error_code& ec) override;

		void set_baud_rate(uint32_t rate, boost::system::error_code& ec) override;
		void set_character_size(uint8_t bits, boost::system::error_code& ec) override;
		void set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec) override;
		void set_parity(Serial::Parity p, boost::system::error_code& ec) override;
		void set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec) override;
		void set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec) override;

		std::size_t read_some(const boost::asio::mutable_buffer& b) override;
		std::size_t read_some(const boost::asio::mutable_buffer& b, boost::system::error_code& ec) override;
		std::size_t write_some(const boost::asio::const_buffer& b) override;
		std::size_t write_some(const boost::asio::const_buffer& b, boost::system::error_code& ec) override;

	private:
		Developer::SerialPortOptions m_Options;

	private:
		boost::asio::any_io_executor m_Executor;
		boost::asio::ip::tcp::socket m_Socket;
		std::string m_EndpointName;
		bool m_IsOpen{ false };

	private:
		bool m_UseRfc2217{ true };
		std::unique_ptr<Interfaces::ISerialPortProtocol> CreateProtocolHandler();
		std::unique_ptr<Interfaces::ISerialPortProtocol> m_ProtocolHandler;

		// Configure a freshly-connected socket (non-blocking mode, TCP_NODELAY) and
		// initialise the protocol handler.  Extracted from open() so the connect
		// success path does not nest control flow too deeply.
		void InitialiseConnectedSocket(const std::string& endpoint_name, boost::system::error_code& ec);

	private:
		Profiling::DomainPtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Serial::PortTypes
