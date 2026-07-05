#include <format>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "utility/jandy_equipment_validator.h"
#include "auxillaries/jandy_powercenter_mapping.h"
#include "kernel/powercenter.h"

namespace AqualinkAutomate::Utility
{

	Kernel::EquipmentValidation ValidateDiscoveredEquipment(
		uint8_t expected_aux,
		uint8_t expected_power_centers,
		const std::vector<Auxillaries::JandyAuxillaryIds>& discovered_aux_ids,
		uint8_t reconfigured_aux_relay_count)
	{
		Kernel::EquipmentValidation result;
		result.ExpectedAuxillaries = expected_aux;
		result.ExpectedPowerCenters = expected_power_centers;

		Kernel::PowerCenters power_centers;
		uint8_t numbered_aux = 0;

		for (const auto id : discovered_aux_ids)
		{
			const auto pc = Auxillaries::PowerCenterForAuxId(id);
			if (!pc.has_value())
			{
				// ExtraAux / unmapped: a dedicated shared relay, not a numbered aux.
				continue;
			}

			++numbered_aux;
			power_centers.Assign(pc.value(), std::string{ magic_enum::enum_name(id) });

			// Span check: an aux attributed to a centre beyond the model's capacity is almost
			// certainly a mis-scrape (e.g. an RS-8 with one centre reporting an "Aux B1").
			if ((expected_power_centers > 0) && (std::to_underlying(pc.value()) >= expected_power_centers))
			{
				result.Anomalies.push_back(std::format(
					"Auxillary '{}' attributed to power center {} but the model has only {} power center(s)",
					magic_enum::enum_name(id),
					static_cast<char>('A' + std::to_underlying(pc.value())),
					expected_power_centers));
			}
		}

		result.DiscoveredAuxillaries = static_cast<uint8_t>(numbered_aux + reconfigured_aux_relay_count);
		result.DiscoveredPowerCenters = power_centers.InstalledCount();

		// Count check (a complete scrape should find exactly the model's aux relays; a
		// shortfall usually means an incomplete/short scrape, an excess means a mis-read).
		if ((expected_aux > 0) && (result.DiscoveredAuxillaries != expected_aux))
		{
			result.Anomalies.push_back(std::format(
				"Discovered {} aux relay(s) ({} numbered + {} reconfigured) but the model expects {}",
				result.DiscoveredAuxillaries, numbered_aux, reconfigured_aux_relay_count, expected_aux));
		}

		// Power-centre count check (the span check above catches an aux beyond capacity; this
		// catches the rarer case where more distinct centres were seen than the model declares).
		if ((expected_power_centers > 0) && (result.DiscoveredPowerCenters > expected_power_centers))
		{
			result.Anomalies.push_back(std::format(
				"Discovered {} power center(s) but the model expects {}",
				result.DiscoveredPowerCenters, expected_power_centers));
		}

		return result;
	}

}
// namespace AqualinkAutomate::Utility
