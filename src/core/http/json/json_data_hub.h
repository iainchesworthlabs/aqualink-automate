#pragma once

#include <nlohmann/json.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"

namespace AqualinkAutomate::Kernel
{
	// Support the translation of the various DEVICE object types to JSON
	//
	// See https://github.com/nlohmann/json#arbitrary-types-conversions.

	void to_json(nlohmann::json& j, const AuxillaryDevice& device);

}
// namespace AqualinkAutomate::Kernel
