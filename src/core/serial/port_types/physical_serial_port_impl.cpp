#include <utility>

#include "serial/port_types/physical_serial_port_impl.h"

namespace AqualinkAutomate::Serial::PortTypes
{

    PhysicalSerialPortImpl::PhysicalSerialPortImpl(boost::asio::any_io_executor executor) :
        m_Executor(std::move(executor)),
        m_SerialPort(m_Executor),
        m_ProfilingDomain(std::move(Factory::ProfilerFactory::Instance().Get()->CreateDomain("PhysicalSerialPortImpl")))
    {
    }

    PhysicalSerialPortImpl::PhysicalSerialPortImpl(boost::asio::any_io_executor executor, const std::string& device_name) :
        PhysicalSerialPortImpl(std::move(executor))
    {
        m_SerialPort.open(device_name);
    }

    PhysicalSerialPortImpl::PhysicalSerialPortImpl(boost::asio::any_io_executor executor, const std::string& device_name, boost::system::error_code& ec) :
        PhysicalSerialPortImpl(std::move(executor))
    {
        m_SerialPort.open(device_name, ec);
    }

    void PhysicalSerialPortImpl::open(const std::string& device_name)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::open", std::source_location::current());
        m_SerialPort.open(device_name);
        // Set a minimal read timeout so read_some returns immediately
        // when no data is available instead of blocking the frame loop.
        boost::system::error_code ec;
        set_read_timeout(std::chrono::milliseconds(0), ec);
        if (ec) throw boost::system::system_error(ec);
    }

    void PhysicalSerialPortImpl::open(const std::string& device_name, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::open", std::source_location::current());
        m_SerialPort.open(device_name, ec);
        if (!ec)
        {
            // Set a minimal read timeout so read_some returns immediately
            // when no data is available instead of blocking the frame loop.
            set_read_timeout(std::chrono::milliseconds(0), ec);
        }
    }

    bool PhysicalSerialPortImpl::is_open() const
    {
        return m_SerialPort.is_open();
    }

    void PhysicalSerialPortImpl::cancel()
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::cancel", std::source_location::current());
        m_SerialPort.cancel();
    }

    void PhysicalSerialPortImpl::cancel(boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::cancel", std::source_location::current());
        m_SerialPort.cancel(ec);
    }

    void PhysicalSerialPortImpl::close()
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::close", std::source_location::current());
        m_SerialPort.close();
    }

    void PhysicalSerialPortImpl::close(boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::close", std::source_location::current());
        m_SerialPort.close(ec);
    }

    void PhysicalSerialPortImpl::set_baud_rate(uint32_t rate, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::set_baud_rate", std::source_location::current());
        m_SerialPort.set_option(boost::asio::serial_port_base::baud_rate(rate), ec);
    }

    void PhysicalSerialPortImpl::set_character_size(uint8_t bits, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::set_character_size", std::source_location::current());
        m_SerialPort.set_option(boost::asio::serial_port_base::character_size(bits), ec);
    }

    void PhysicalSerialPortImpl::set_flow_control(Serial::FlowControl fc, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::set_flow_control", std::source_location::current());
        m_SerialPort.set_option(boost::asio::serial_port_base::flow_control(
            static_cast<boost::asio::serial_port_base::flow_control::type>(std::to_underlying(fc))), ec);
    }

    void PhysicalSerialPortImpl::set_parity(Serial::Parity p, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::set_parity", std::source_location::current());
        m_SerialPort.set_option(boost::asio::serial_port_base::parity(
            static_cast<boost::asio::serial_port_base::parity::type>(std::to_underlying(p))), ec);
    }

    void PhysicalSerialPortImpl::set_stop_bits(Serial::StopBits sb, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::set_stop_bits", std::source_location::current());
        m_SerialPort.set_option(boost::asio::serial_port_base::stop_bits(
            static_cast<boost::asio::serial_port_base::stop_bits::type>(std::to_underlying(sb))), ec);
    }

    std::size_t PhysicalSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::read_some", std::source_location::current());
        return m_SerialPort.read_some(boost::asio::buffer(buffer));
    }

    std::size_t PhysicalSerialPortImpl::read_some(const boost::asio::mutable_buffer& buffer, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::read_some", std::source_location::current());
        return m_SerialPort.read_some(boost::asio::buffer(buffer), ec);
    }

    std::size_t PhysicalSerialPortImpl::write_some(const boost::asio::const_buffer& buffer)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::write_some", std::source_location::current());
        return m_SerialPort.write_some(boost::asio::buffer(buffer));
    }

    std::size_t PhysicalSerialPortImpl::write_some(const boost::asio::const_buffer& buffer, boost::system::error_code& ec)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PhysicalSerialPortImpl::write_some", std::source_location::current());
        return m_SerialPort.write_some(boost::asio::buffer(buffer), ec);
    }

}
// namespace AqualinkAutomate::Serial::PortTypes
