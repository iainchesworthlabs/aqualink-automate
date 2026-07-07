#pragma once

#include <chrono>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/serial_port.hpp>
#include <boost/system/error_code.hpp>

#include "interfaces/iserialportimpl.h"
#include "profiling/profiling.h"

namespace AqualinkAutomate::Serial::PortTypes
{

	class PhysicalSerialPortImpl : public Interfaces::ISerialPortImpl
	{
	public:
		explicit PhysicalSerialPortImpl(boost::asio::any_io_executor executor);
		PhysicalSerialPortImpl(boost::asio::any_io_executor executor, const std::string& device_name);
		PhysicalSerialPortImpl(boost::asio::any_io_executor executor, const std::string& device_name, boost::system::error_code& ec);

		~PhysicalSerialPortImpl() override = default;

		PhysicalSerialPortImpl(const PhysicalSerialPortImpl&) = delete;
		PhysicalSerialPortImpl& operator=(const PhysicalSerialPortImpl&) = delete;
		PhysicalSerialPortImpl(PhysicalSerialPortImpl&& other) noexcept = delete;
		PhysicalSerialPortImpl& operator=(PhysicalSerialPortImpl&& other) noexcept = delete;

		void open(const std::string& device_name) override;
		void open(const std::string& device_name, boost::system::error_code& ec) override;
		bool is_open() const override;
		void cancel() override;
		void cancel(boost::system::error_code& ec) override;
		void close() override;
		void close(boost::system::error_code& ec) override;

		void set_baud_rate(uint32_t rate, boost::system::error_code& ec) override;
		void set_character_size(uint8_t bits, boost::system::error_code& ec) override;
		void set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec) override;
		void set_parity(Serial::Parity p, boost::system::error_code& ec) override;
		void set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec) override;
		void set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec) override;

		std::size_t read_some(const boost::asio::mutable_buffer& b) override;
		std::size_t read_some(const boost::asio::mutable_buffer& buffer, boost::system::error_code& ec) override;
		std::size_t write_some(const boost::asio::const_buffer& buffer) override;
		std::size_t write_some(const boost::asio::const_buffer& buffer, boost::system::error_code& ec) override;

	private:
		boost::asio::any_io_executor m_Executor;
		boost::asio::serial_port m_SerialPort;
		Profiling::DomainPtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Serial::PortTypes
