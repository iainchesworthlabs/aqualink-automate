#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "application/application_defaults.h"
#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Serial
{

	typedef struct tagSerialSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Serial" };
			return AREA_NAME;
		}

		std::string serial_port{ "/dev/ttyUSB0" };
		uint16_t baud_rate{ 9600 };

		std::string remote_serial_port;
		bool use_rfc2217{ false };
		bool use_rawtcp{ false };

		bool UsingPhysicalSerialPort() const
		{
			return !UsingRemoteSerialPort();
		}

		bool UsingRemoteSerialPort() const
		{
			return !remote_serial_port.empty();
		}

	}
	SerialSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_SERIALPORT{ make_appoption("serial-port", "s", "Serial port to use for Aqualink connectivity", boost::program_options::value<std::string>()->default_value(Application::SERIAL_PORT)) };
		AppOptionPtr OPTION_BAUDRATE{ make_appoption("baudrate", "Desired serial port baud rate setting", boost::program_options::value<uint16_t>()->default_value(9600)) };
		AppOptionPtr OPTION_REMOTESERIALPORT{ make_appoption("remote-serial-port", "r", "Remote serial port to use for Aqualink connectivity", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_USE_RFC2217{ make_appoption("rfc2217", "Use RFC2217 with the remote serial port (the default for remote ports)", boost::program_options::bool_switch()->default_value(true)) };
		AppOptionPtr OPTION_NO_RFC2217{ make_appoption("no-rfc2217", "Disable RFC2217 for the remote serial port (use a plain socket)", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_USE_RAWTCP{ make_appoption("rawtcp", "Use raw TCP with the remote serial port", boost::program_options::bool_switch()->default_value(false)) };

		const std::vector<AppOptionPtr> SerialOptionsCollection
		{
			OPTION_SERIALPORT,
			OPTION_BAUDRATE,
			OPTION_REMOTESERIALPORT,
			OPTION_USE_RFC2217,
			OPTION_NO_RFC2217,
			OPTION_USE_RAWTCP
		};

	public:
		using SettingsType = SerialSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Serial
