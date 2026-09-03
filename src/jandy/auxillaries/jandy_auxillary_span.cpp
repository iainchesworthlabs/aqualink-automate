#include "auxillaries/jandy_auxillary_span.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "auxillaries/jandy_auxillary_presence_override.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "auxillaries/jandy_powercenter_mapping.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/device_graph/device_graph.h"
#include "kernel/system_boards.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auxillaries
{

	AuxillaryModelSpan::AuxillaryModelSpan(uint8_t power_centers, uint8_t auxillary_count) :
		m_PowerCenters(power_centers),
		m_AuxillaryCount(auxillary_count)
	{
	}

	AuxillaryModelSpan AuxillaryModelSpan::FromDataHub(const Kernel::DataHub& data_hub)
	{
		// Both conditions matter. PoolConfigurationDecoder falls back to a placeholder entry
		// (1 aux / 1 power centre) for a panel type it cannot decode, so the counts alone would
		// wrongly trim a real multi-centre panel whose type string is simply unrecognised.
		// Requiring an identified SystemBoard is what separates "the model says one centre" from
		// "we could not read the model".
		if ((Kernel::SystemBoards::Unknown == data_hub.SystemBoard) || (0 == data_hub.ExpectedPowerCenterCount))
		{
			return AuxillaryModelSpan{};
		}

		return AuxillaryModelSpan{ data_hub.ExpectedPowerCenterCount, data_hub.ExpectedAuxillaryCount };
	}

	bool AuxillaryModelSpan::IsKnown() const
	{
		return (0 != m_PowerCenters);
	}

	bool AuxillaryModelSpan::Contains(JandyAuxillaryIds id) const
	{
		if (!IsKnown())
		{
			// Model not identified yet: never trim.
			return true;
		}

		const auto power_center = PowerCenterForAuxId(id);
		if (!power_center.has_value())
		{
			// ExtraAux (0x00): a dedicated shared relay belonging to no numbered power centre,
			// so a power-centre span has no opinion about it and "no opinion" must mean KEEP.
			// It is a real relay on panels that have one (the AquaLink RS manual describes it as
			// the solar booster pump whenever solar heating is enabled), and the model tables
			// count only numbered relays, so nothing here can tell an absent one from a present
			// one. Deciding that needs the separate, capture-gated ExtraAux investigation -- not
			// a silent deletion off the back of a relay-count table.
			return true;
		}

		// Span check: an aux belonging to a power centre the model does not have.
		if (std::to_underlying(power_center.value()) >= m_PowerCenters)
		{
			return false;
		}

		// Within-centre check, SINGLE-CENTRE MODELS ONLY. ExpectedAuxillaryCount is a TOTAL
		// across centres and dual-equipment models split relays unevenly between them (RS-2/10
		// is A=6 + B=4, not A=7 + B=3), so the total only bounds an individual centre when
		// there is exactly one. For those models the id is the relay number, so an id above the
		// count is a relay the panel does not have (e.g. Aux5 on a 3-relay RS-4).
		if ((1 == m_PowerCenters) && (0 != m_AuxillaryCount) && (std::to_underlying(id) > m_AuxillaryCount))
		{
			return false;
		}

		return true;
	}

	namespace
	{
		// Recover the aux id a device identifies as: the protocol trait first, then the immutable
		// hardware label ("Aux B1"), then a still-generic display label. A device that resolves to
		// none of these is not identifiably an aux relay and is left alone.
		std::optional<JandyAuxillaryIds> ResolveAuxId(const std::shared_ptr<Kernel::AuxillaryDevice>& device)
		{
			if (auto id = device->AuxillaryTraits.TryGet(JandyAuxillaryId{}); id.has_value())
			{
				return id.value();
			}

			if (auto hardware_label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::HardwareLabelTrait{}); hardware_label.has_value())
			{
				if (auto id = ParseAuxId(std::string_view{ hardware_label.value() }); id.has_value())
				{
					return id;
				}
			}

			if (auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label.has_value())
			{
				return ParseAuxId(std::string_view{ label.value() });
			}

			return std::nullopt;
		}

		// Has an operator-assigned name, i.e. one that does NOT parse back to an aux id. A device
		// still labelled "Aux B1" was never named by anything; "Garden Lights" was.
		bool HasCustomLabel(const std::shared_ptr<Kernel::AuxillaryDevice>& device)
		{
			auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			return label.has_value() && !ParseAuxId(std::string_view{ label.value() }).has_value();
		}
	}
	// unnamed namespace

	std::size_t PruneAuxillariesOutsideSpan(Kernel::DevicesGraph& devices, const AuxillaryModelSpan& span)
	{
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait;
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypes;

		if (!span.IsKnown())
		{
			return 0;
		}

		std::size_t removed = 0;

		// FindByTrait returns a snapshot vector, so removing from the graph mid-iteration is safe.
		for (const auto& device : devices.FindByTrait(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary))
		{
			if (nullptr == device)
			{
				continue;
			}

			const auto aux_id = ResolveAuxId(device);
			if (!aux_id.has_value() || span.Contains(aux_id.value()))
			{
				continue;
			}

			// Safety valve: a relay carrying an operator-assigned label was NAMED by something
			// that enumerates real equipment (the iAQ aux list, or the OneTouch Label-Aux
			// scrape) -- that is positive evidence it exists, and it outranks a model table we
			// may simply have wrong for this panel. A phantom minted by a blind status sweep has
			// no label at all and falls back to the generic enum name, which is what makes the
			// two distinguishable. Keep the named one; the equipment validator already reports
			// it as an anomaly rather than silently deleting a device someone can see.
			if (HasCustomLabel(device))
			{
				LogWarning(Channel::Equipment, std::format("Auxillary '{}' is outside the detected panel model's relay span but carries the custom label '{}'; keeping it",
					magic_enum::enum_name(aux_id.value()),
					device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}).value_or(std::string{})));
				continue;
			}

			LogInfo(Channel::Equipment, std::format("Removing auxillary '{}': the detected panel model has no such relay", magic_enum::enum_name(aux_id.value())));

			devices.Remove(device);
			++removed;
		}

		return removed;
	}

	std::size_t ClearAutoDetectedAuxillaries(Kernel::DevicesGraph& devices, const nlohmann::json& presence_overrides)
	{
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait;
		using Kernel::AuxillaryTraitsTypes::AuxillaryTypes;

		std::size_t removed = 0;

		// FindByTrait returns a snapshot vector, so removing from the graph mid-iteration is safe.
		for (const auto& device : devices.FindByTrait(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary))
		{
			if (nullptr == device)
			{
				continue;
			}

			const auto aux_id = ResolveAuxId(device);
			if (aux_id.has_value() && IsForcedPresent(aux_id.value(), presence_overrides))
			{
				// An operator-forced device is a deliberate declaration, not something
				// auto-detection produced -- clearing it here would just have it recreated by
				// the next override reconciliation, discarding its live status meanwhile.
				continue;
			}

			devices.Remove(device);
			++removed;
		}

		return removed;
	}

}
// namespace AqualinkAutomate::Auxillaries
