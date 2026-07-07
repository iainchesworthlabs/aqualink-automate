#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include "serial/serial_port_enums.h"

namespace AqualinkAutomate::Interfaces
{

    class ISerialPortImpl
    {
    public:
        ISerialPortImpl() = default;
        virtual ~ISerialPortImpl() = default;

        ISerialPortImpl(const ISerialPortImpl&) = delete;
        ISerialPortImpl& operator=(const ISerialPortImpl&) = delete;
        ISerialPortImpl(ISerialPortImpl&& other) noexcept = delete;
        ISerialPortImpl& operator=(ISerialPortImpl&& other) = delete;

        virtual void open(const std::string& device) = 0;
        virtual void open(const std::string& device, boost::system::error_code& ec) = 0;
        virtual bool is_open() const = 0;
        virtual void cancel() = 0;
        virtual void cancel(boost::system::error_code& ec) = 0;
        virtual void close() = 0;
        virtual void close(boost::system::error_code& ec) = 0;

        virtual void set_baud_rate(uint32_t rate, boost::system::error_code& ec) = 0;
        virtual void set_character_size(uint8_t bits, boost::system::error_code& ec) = 0;
        virtual void set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec) = 0;
        virtual void set_parity(Serial::Parity p, boost::system::error_code& ec) = 0;
        virtual void set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec) = 0;
        virtual void set_read_timeout(std::chrono::milliseconds timeout, boost::system::error_code& ec) = 0;

        virtual std::size_t read_some(const boost::asio::mutable_buffer& b) = 0;
        virtual std::size_t read_some(const boost::asio::mutable_buffer& b, boost::system::error_code& ec) = 0;
        virtual std::size_t write_some(const boost::asio::const_buffer& b) = 0;
        virtual std::size_t write_some(const boost::asio::const_buffer& b, boost::system::error_code& ec) = 0;

    };

}
// namespace AqualinkAutomate::Interfaces
