#include "auxillaries/jandy_auxillary_reconciliation.h"

#include <optional>
#include <string>

#include <magic_enum/magic_enum.hpp>

#include "auxillaries/jandy_auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/device_graph/device_graph.h"

namespace AqualinkAutomate::Auxillaries
{

	void EnsureAuxIdentity(const std::shared_ptr<Kernel::AuxillaryDevice>& device, JandyAuxillaryIds id)
	{
		if (nullptr == device)
		{
			return;
		}

		if (!device->AuxillaryTraits.Has(JandyAuxillaryId{}))
		{
			device->AuxillaryTraits.Set(JandyAuxillaryId{}, id);
		}

		if (!device->AuxillaryTraits.Has(Kernel::AuxillaryTraitsTypes::HardwareLabelTrait{}))
		{
			device->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HardwareLabelTrait{}, std::string{ magic_enum::enum_name(id) });
		}
	}

	namespace
	{
		std::optional<std::string> LabelOf(const std::shared_ptr<Kernel::AuxillaryDevice>& device)
		{
			if (auto l = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); l.has_value())
			{
				return std::string{ l.value() };
			}
			return std::nullopt;
		}

		// A label is "custom" (operator-assigned) when it does NOT parse back to an aux id,
		// i.e. it is something like "Pool Light" rather than the generic "Aux5".
		bool IsCustomLabel(const std::optional<std::string>& label)
		{
			return label.has_value() && !ParseAuxId(label.value()).has_value();
		}
	}
	// unnamed namespace

	void RemoveOrphanAuxPlaceholders(Kernel::DevicesGraph& devices, JandyAuxillaryIds aux_id, const std::shared_ptr<Kernel::AuxillaryDevice>& keep)
	{
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait;
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypes;
		using Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait;
		using Kernel::AuxillaryTraitsTypes::HardwareLabelTrait;
		using Kernel::AuxillaryTraitsTypes::LabelTrait;

		if (nullptr == keep)
		{
			return;
		}

		auto keep_label = LabelOf(keep);
		bool keep_has_custom_label = IsCustomLabel(keep_label);

		// FindByTrait returns a snapshot vector, so removing from the graph mid-iteration is safe.
		for (const auto& orphan : devices.FindByTrait(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary))
		{
			if (nullptr == orphan || orphan == keep)
			{
				continue;
			}
			// Only prune cache placeholders: an auxillary with no stable aux identity.
			if (orphan->AuxillaryTraits.Has(JandyAuxillaryId{}))
			{
				continue;
			}

			// Does this placeholder represent the same physical aux as `keep`? Match on the aux
			// identity recoverable from its label or hardware label, falling back to a shared label.
			const auto orphan_label = LabelOf(orphan);
			bool belongs = (orphan_label.has_value() && ParseAuxId(orphan_label.value()) == aux_id);
			if (!belongs)
			{
				if (auto hw = orphan->AuxillaryTraits.TryGet(HardwareLabelTrait{});
					hw.has_value() && ParseAuxId(std::string_view{ hw.value() }) == aux_id)
				{
					belongs = true;
				}
			}
			if (!belongs && keep_label.has_value() && orphan_label == keep_label)
			{
				belongs = true;
			}
			if (!belongs)
			{
				continue;
			}

			// Preserve a cached custom label / body the live device does not yet carry, so pruning
			// the placeholder before the Label-Aux page is seen does not lose enumerated info.
			if (!keep_has_custom_label && orphan_label.has_value() && IsCustomLabel(orphan_label))
			{
				keep->AuxillaryTraits.Set(LabelTrait{}, orphan_label.value());
				keep_label = orphan_label;
				keep_has_custom_label = true;
			}
			if (!keep->AuxillaryTraits.Has(BodyOfWaterTrait{}))
			{
				if (auto body = orphan->AuxillaryTraits.TryGet(BodyOfWaterTrait{}); body.has_value())
				{
					keep->AuxillaryTraits.Set(BodyOfWaterTrait{}, body.value());
				}
			}

			devices.Remove(orphan);
		}
	}

}
// namespace AqualinkAutomate::Auxillaries
