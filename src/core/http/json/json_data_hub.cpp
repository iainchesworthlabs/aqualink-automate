#include <format>

#include <boost/uuid/uuid_io.hpp>
#include <magic_enum/magic_enum.hpp>

#include "http/json/json_data_hub.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{

	void to_json(nlohmann::json& j, const AuxillaryDevice& device)
	{
		const auto device_id_str = boost::uuids::to_string(device.Id());
		LogTrace(Channel::Web, [&device_id_str] { return std::format("Converting AuxillaryDevice {} to JSON", device_id_str); });

		nlohmann::json json_payload;

		json_payload["id"] = device_id_str;

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::LabelTrait{}))
		{
			const auto label = *(device.AuxillaryTraits[AuxillaryTraitsTypes::LabelTrait{}]);
			json_payload["label"] = label;
			LogTrace(Channel::Web, [&device_id_str, &label] { return std::format("  Device {} label: '{}'", device_id_str, label); });
		}
		else
		{
			LogTrace(Channel::Web, [&device_id_str] { return std::format("  Device {} has no label trait", device_id_str); });
		}

		const auto state = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(device);
		json_payload["state"] = state;
		LogTrace(Channel::Web, [&device_id_str, &state] { return std::format("  Device {} state: {}", device_id_str, state); });

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::GeneratingPercentageTrait{}))
		{
			json_payload["generating_percentage"] = *(device.AuxillaryTraits[AuxillaryTraitsTypes::GeneratingPercentageTrait{}]);
		}

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::BoostModeTrait{}))
		{
			json_payload["boost_mode"] = std::string(magic_enum::enum_name(*(device.AuxillaryTraits[AuxillaryTraitsTypes::BoostModeTrait{}])));
		}

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::ChlorinatorHealthTrait{}))
		{
			json_payload["chlorinator_health"] = std::string(magic_enum::enum_name(*(device.AuxillaryTraits[AuxillaryTraitsTypes::ChlorinatorHealthTrait{}])));
		}

		// Every health flag currently active simultaneously (the wire status byte is a true
		// bitfield); "chlorinator_health" above remains the single worst-of-the-set value.
		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::ChlorinatorHealthFlagsTrait{}))
		{
			nlohmann::json health_flags = nlohmann::json::array();
			for (const auto flag : *(device.AuxillaryTraits[AuxillaryTraitsTypes::ChlorinatorHealthFlagsTrait{}]))
			{
				health_flags.push_back(std::string(magic_enum::enum_name(flag)));
			}
			json_payload["chlorinator_health_flags"] = std::move(health_flags);
		}

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::DutyCycleTrait{}))
		{
			json_payload["duty_cycle"] = *(device.AuxillaryTraits[AuxillaryTraitsTypes::DutyCycleTrait{}]);
		}

		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::BodyOfWaterTrait{}))
		{
			json_payload["body_of_water"] = std::string(magic_enum::enum_name(*(device.AuxillaryTraits[AuxillaryTraitsTypes::BodyOfWaterTrait{}])));
		}

		// The protocol-native short id ("Aux5") - exposed so the UI can show
		// "friendly name (aux id)" and resolve a device by its hardware label.
		if (device.AuxillaryTraits.Has(AuxillaryTraitsTypes::HardwareLabelTrait{}))
		{
			json_payload["hardware_id"] = std::string{ *(device.AuxillaryTraits[AuxillaryTraitsTypes::HardwareLabelTrait{}]) };
		}

		j = json_payload;
	}

}
// namespace AqualinkAutomate::Kernel
