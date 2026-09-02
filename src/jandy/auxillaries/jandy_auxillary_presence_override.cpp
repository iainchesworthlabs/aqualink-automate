#include "auxillaries/jandy_auxillary_presence_override.h"

#include <format>
#include <string>

#include <magic_enum/magic_enum.hpp>

#include "auxillaries/jandy_auxillary_traits_types.h"
#include "factories/jandy_auxillary_factory.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/device_graph/device_graph.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auxillaries
{

	AuxPresenceOverride GetPresenceOverride(JandyAuxillaryIds id, const nlohmann::json& overrides)
	{
		if (!overrides.is_object())
		{
			return AuxPresenceOverride::Auto;
		}

		const auto key = std::string{ magic_enum::enum_name(id) };
		const auto it = overrides.find(key);
		if ((overrides.end() == it) || !it->is_string())
		{
			return AuxPresenceOverride::Auto;
		}

		const auto& value = it->get_ref<const std::string&>();
		if ("present" == value)
		{
			return AuxPresenceOverride::Present;
		}
		if ("absent" == value)
		{
			return AuxPresenceOverride::Absent;
		}

		return AuxPresenceOverride::Auto;
	}

	std::size_t ApplyPresenceOverrides(Kernel::DevicesGraph& devices, const nlohmann::json& overrides)
	{
		if (!overrides.is_object())
		{
			return 0;
		}

		std::size_t changed = 0;

		for (const auto& [key, value] : overrides.items())
		{
			if (!value.is_string())
			{
				continue;
			}

			const auto aux_id = ParseAuxId(key);
			if (!aux_id.has_value())
			{
				continue;
			}

			const auto& override_value = value.get_ref<const std::string&>();
			const auto stable_id = AuxStableId(aux_id.value());

			if ("present" == override_value)
			{
				if (nullptr != devices.FindById(stable_id))
				{
					continue;
				}

				auto new_device = Factory::JandyAuxillaryFactory::Instance().SerialAdapterDevice_CreateDevice(aux_id.value());
				if (!new_device.has_value())
				{
					LogWarning(Channel::Equipment, std::format("Failed to synthesize forced-present auxillary '{}': {}", key, new_device.error().message()));
					continue;
				}

				// No wire evidence backs this device at all -- it exists purely because the
				// operator forced it. Mark it so the UI can tell "forced but not yet independently
				// confirmed" apart from "the bus actually detected this" (see
				// GenerateJson_Equipment_AuxSlots); every evidence-driven creation/update path
				// clears this the first time it touches the device for real.
				new_device.value()->AuxillaryTraits.Set(SynthesizedTrait{}, true);

				devices.Add(new_device.value());
				++changed;
			}
			else if ("absent" == override_value)
			{
				if (auto existing = devices.FindById(stable_id); nullptr != existing)
				{
					devices.Remove(existing);
					++changed;
				}
			}
		}

		return changed;
	}

}
// namespace AqualinkAutomate::Auxillaries
