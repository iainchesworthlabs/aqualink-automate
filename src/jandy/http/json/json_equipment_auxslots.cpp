#include "http/json/json_equipment_auxslots.h"

#include <string>

#include <boost/uuid/uuid_io.hpp>
#include <magic_enum/magic_enum.hpp>

#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_presence_override.h"
#include "auxillaries/jandy_auxillary_span.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "auxillaries/jandy_powercenter_mapping.h"
#include "http/json/json_equipment.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/powercenter.h"

namespace AqualinkAutomate::HTTP::JSON
{

	nlohmann::json GenerateJson_Equipment_AuxSlots(const std::shared_ptr<Kernel::DataHub>& data_hub, const nlohmann::json& label_overrides, bool show_aux_id_in_label, const nlohmann::json& presence_overrides)
	{
		nlohmann::json slots = nlohmann::json::array();

		const auto span = Auxillaries::AuxillaryModelSpan::FromDataHub(*data_hub);

		for (const auto aux_id : magic_enum::enum_values<Auxillaries::JandyAuxillaryIds>())
		{
			const std::string hardware_id{ magic_enum::enum_name(aux_id) };

			nlohmann::json slot;
			slot["aux_id"] = hardware_id;

			if (const auto power_centre = Auxillaries::PowerCenterForAuxId(aux_id); power_centre.has_value())
			{
				slot["power_centre"] = std::string{ magic_enum::enum_name(power_centre.value()) };
			}
			else
			{
				slot["power_centre"] = nullptr;
			}

			slot["in_model_span"] = span.Contains(aux_id);

			switch (Auxillaries::GetPresenceOverride(aux_id, presence_overrides))
			{
			case Auxillaries::AuxPresenceOverride::Present:
				slot["presence_override"] = "present";
				break;
			case Auxillaries::AuxPresenceOverride::Absent:
				slot["presence_override"] = "absent";
				break;
			case Auxillaries::AuxPresenceOverride::Auto:
			default:
				slot["presence_override"] = "auto";
				break;
			}

			const auto device = data_hub->Devices.FindById(Auxillaries::AuxStableId(aux_id));

			// A device object existing in the graph is not, on its own, proof the bus has
			// confirmed this aux -- ApplyPresenceOverrides synthesizes one from a "force present"
			// override with no wire evidence at all, marking it with SynthesizedTrait. Only an
			// organically-detected device (trait absent, or explicitly cleared once real evidence
			// arrives) counts as "detected"; a synthesized-but-unconfirmed one instead surfaces
			// via presence_override so the UI can show its own "Forced - not seen" state.
			bool synthesized = false;
			if (nullptr != device)
			{
				if (const auto flag = device->AuxillaryTraits.TryGet(Auxillaries::SynthesizedTrait{}); flag.has_value())
				{
					synthesized = flag.value();
				}
			}
			slot["detected"] = (nullptr != device) && !synthesized;

			if (nullptr != device)
			{
				slot["device_id"] = boost::uuids::to_string(device->Id());

				if (device->AuxillaryTraits.Has(Kernel::AuxillaryTraitsTypes::LabelTrait{}))
				{
					const std::string label = *(device->AuxillaryTraits[Kernel::AuxillaryTraitsTypes::LabelTrait{}]);
					slot["label"] = label;
					slot["display_label"] = ComputeDisplayLabel(label, hardware_id, label_overrides, show_aux_id_in_label);
				}

				if (Kernel::AuxillaryTraitsTypes::HasStatus(device))
				{
					slot["status"] = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(device);
				}
			}

			slots.push_back(std::move(slot));
		}

		return slots;
	}

}
// namespace AqualinkAutomate::HTTP::JSON
