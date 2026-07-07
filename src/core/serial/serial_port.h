#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "interfaces/iserialport.h"
#include "interfaces/iserialportimpl.h"
#include "kernel/hub_locator.h"
#include "kernel/statistics_hub.h"
#include "profiling/profiling.h"
#include "serial/serial_port_enums.h"

namespace AqualinkAutomate::Serial
{

	class SerialPort : public Interfaces::ISerialPort, public std::enable_shared_from_this<SerialPort>
	{
	public:
		explicit SerialPort(std::unique_ptr<Interfaces::ISerialPortImpl> serial_port_impl, Kernel::HubLocator& hub_locator);

		void open(const std::string& device);
		void open(const std::string& device, boost::system::error_code& ec);
		bool is_open() const;
		void cancel();
		void cancel(boost::system::error_code& ec);
		void close();
		void close(boost::system::error_code& ec);

		void set_baud_rate(uint32_t rate);
		void set_baud_rate(uint32_t rate, boost::system::error_code& ec);
		void set_character_size(uint8_t bits);
		void set_character_size(uint8_t bits, boost::system::error_code& ec);
		void set_flow_control(FlowControl fc);
		void set_flow_control(FlowControl fc, boost::system::error_code& ec);
		void set_parity(Parity p);
		void set_parity(Parity p, boost::system::error_code& ec);
		void set_stop_bits(StopBits sb);
		void set_stop_bits(StopBits sb, boost::system::error_code& ec);
		void set_read_timeout(std::chrono::milliseconds timeout);
		void set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec);

		std::size_t read_some(const boost::asio::mutable_buffer& buffers);
		std::size_t read_some(const boost::asio::mutable_buffer& buffers, boost::system::error_code& ec);
		std::size_t write_some(const boost::asio::const_buffer& buffers);
		std::size_t write_some(const boost::asio::const_buffer& buffers, boost::system::error_code& ec);

	private:
		std::unique_ptr<Interfaces::ISerialPortImpl> m_SerialPortImpl;

		std::shared_ptr<Kernel::StatisticsHub> m_StatisticsHub;
	};

}
// namespace AqualinkAutomate::Serial
