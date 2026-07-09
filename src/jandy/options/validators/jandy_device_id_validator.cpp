#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>

#include <boost/program_options.hpp>

#include "logging/logging.h"
#include "options/validators/jandy_device_id_validator.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	void validate(boost::any& v, std::vector<std::string> const& values, JandyDeviceId* /*target_type*/, int)
	{
		static const uint8_t MINIMUM_DEVICE_ID_LENGTH = 3;
		static const uint8_t MAXIMUM_DEVICE_ID_LENGTH = 4;

		static const char HEXADECIMAL_SEPARATOR_CHAR = 'x';
		static const uint8_t DEVICE_ID_OFFSET_IN_VALUE = 2;

		std::string device_id_string;
		uint32_t temporary_device_id = 0;

		boost::program_options::validators::check_first_occurrence(v);
		device_id_string = boost::program_options::validators::get_single_string(values);
		
		if (MINIMUM_DEVICE_ID_LENGTH > device_id_string.length())
		{
			LogDebug(Channel::Main, std::format("Invalid conversion of emulated device id: device id was too short -> {}", device_id_string));
			throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
		}
		else if (MAXIMUM_DEVICE_ID_LENGTH < device_id_string.length())
		{
			LogDebug(Channel::Main, std::format("Invalid conversion of emulated device id: device id was too long -> {}", device_id_string));
			throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
		}
		else if (HEXADECIMAL_SEPARATOR_CHAR != std::tolower(device_id_string[1]))
		{
			LogDebug(Channel::Main, std::format("Invalid conversion of emulated device id: device id was incorrectly formatted -> {}", device_id_string));
			throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
		}
		else
		{
			const auto device_id_from_offset = device_id_string.substr(DEVICE_ID_OFFSET_IN_VALUE);
			auto [ptr, ec] = std::from_chars(device_id_from_offset.data(), device_id_from_offset.data() + device_id_from_offset.size(), temporary_device_id, 16);

			if (std::errc() != ec || (device_id_from_offset.data() + device_id_from_offset.size()) != ptr)
			{
				LogDebug(Channel::Main, std::format("Invalid conversion of emulated device id: provided string was invalid -> {}", device_id_string));
				throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
			}
			else if (std::numeric_limits<uint8_t>::max() < temporary_device_id)
			{
				LogDebug(Channel::Main, std::format("Invalid conversion of emulated device id: provided value was too large -> {}", device_id_string));
				throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
			}
			else
			{
				JandyDeviceId device_id(static_cast<uint8_t>(temporary_device_id));
				v = boost::any(device_id);
			}
		}
	}

}
// namespace AqualinkAutomate::Devices
