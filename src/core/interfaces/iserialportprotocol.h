#pragma once

#include <boost/system/error_code.hpp>

#include "serial/serial_port_enums.h"

namespace AqualinkAutomate::Interfaces
{

	class ISerialPortProtocol
	{
	public:
		ISerialPortProtocol() = default;
		virtual ~ISerialPortProtocol() = default;

		virtual void Initialize() = 0;
		virtual void Shutdown() = 0;

		virtual void set_baud_rate(uint32_t baud_rate) = 0;
		virtual void set_character_size(uint8_t size) = 0;
		virtual void set_parity(Serial::Parity parity) = 0;
		virtual void set_stop_bits(Serial::StopBits stop_bits) = 0;
		virtual void set_flow_control(Serial::FlowControl flow_control) = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
