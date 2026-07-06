#include <format>
#include <utility>
#include <vector>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_startup_survey.h"
#include "devices/jandy_device_types.h"
#include "formatters/jandy_device_formatters.h"
#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "navigation/menu_model.h"
#include "navigation/onetouch_menu_model.h"
#include "navigation/spider_engine.h"
#include "navigation/visit_policies.h"
#include "utility/jandy_equipment_validator.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices::OneTouch
{

	bool DataHubHasSeededAuxLabels(const Kernel::DataHub& data_hub)
	{
		// A real iAqualink2 (AqualinkTouch 0x33) decodes aux NAMES from its AuxStatus (0x72) frames
		// and sets LabelTrait on the matching DataHub aux devices passively. Any aux device carrying
		// a non-empty label is evidence the labels are already known, so the emulated OneTouch can
		// skip scraping them. Keyed by JandyAuxillaryId so only Jandy auxes are considered.
		for (const auto& device : data_hub.Devices.FindByTrait(Auxillaries::JandyAuxillaryId{}))
		{
			if (nullptr == device)
			{
				continue;
			}

			if (auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
				label.has_value() && !Utility::TrimWhitespace(label.value()).empty())
			{
				return true;
			}
		}

		return false;
	}

	bool DataHubChlorinatorOnline(const Kernel::DataHub& data_hub)
	{
		using namespace Kernel::AuxillaryTraitsTypes;

		auto chlorinators = data_hub.Chlorinators();
		if (chlorinators.empty())
		{
			return false;
		}

		const auto& device = chlorinators.front();
		if (!device->AuxillaryTraits.Has(ChlorinatorStatusTrait{}))
		{
			return false;
		}

		const auto status = *(device->AuxillaryTraits[ChlorinatorStatusTrait{}]);
		return (status != Kernel::ChlorinatorStatuses::Off) && (status != Kernel::ChlorinatorStatuses::Unknown);
	}

	void ValidateDiscoveredEquipment(Kernel::DataHub& data_hub, const Devices::JandyDeviceType& device_id)
	{
		// Gather the Jandy ids of every numbered auxillary that was discovered.
		std::vector<Auxillaries::JandyAuxillaryIds> discovered_aux_ids;
		for (const auto& aux : data_hub.Auxillaries())
		{
			if (aux && aux->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}))
			{
				discovered_aux_ids.push_back(aux->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]);
			}
		}

		// Equipment occupying an aux relay that is NOT a numbered aux because an IO-board DIP switch
		// repurposed the relay (cleaner / spillover / sprinkler). Counted toward the relay total so a
		// DIP-repurposed panel still validates against the model's aux count.
		const auto reconfigured_aux_relays = static_cast<uint8_t>(
			data_hub.CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Cleaner)
			+ data_hub.CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Spillover)
			+ data_hub.CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Sprinkler));

		auto result = Utility::ValidateDiscoveredEquipment(
			data_hub.ExpectedAuxillaryCount,
			data_hub.ExpectedPowerCenterCount,
			discovered_aux_ids,
			reconfigured_aux_relays);

		if (result.ExpectedAuxillaries == 0)
		{
			// The version page was never scraped (no model decoded) - nothing to validate against.
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Skipping equipment validation - model not yet decoded", device_id));
		}
		else if (result.Passed())
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Equipment validated - {} aux relay(s) across {} power center(s) match the model",
				device_id, result.DiscoveredAuxillaries, result.DiscoveredPowerCenters));
		}
		else
		{
			for (const auto& anomaly : result.Anomalies)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): Equipment validation anomaly - {}", device_id, anomaly));
			}
		}

		data_hub.EquipmentValidationResult = std::move(result);
	}

	MenuSurveyResult BuildMenuSurvey(const Navigation::SpiderEngine& engine, const Navigation::MenuModel& model, const Devices::JandyDeviceType& device_id)
	{
		const auto& visited = engine.GetVisitedPages();
		const auto& failed = engine.GetFailedPages();

		MenuSurveyResult survey;
		survey.PagesReached = static_cast<uint32_t>(visited.size() - failed.size());
		survey.EquipmentPageReached = visited.contains(Navigation::PageId::EquipmentOnOff)
			&& !failed.contains(Navigation::PageId::EquipmentOnOff);

		for (const auto page : failed)
		{
			const auto* page_info = model.GetPage(page);
			const std::string name = page_info ? page_info->name : std::format("page {}", std::to_underlying(page));

			if (auto requirement = Navigation::OneTouchPageCapabilityRequirement(page); requirement.has_value())
			{
				survey.ExpectedAbsent.push_back(std::format("{} ({})", name, requirement.value()));
			}
			else
			{
				survey.NotableFailures.push_back(name);
			}
		}

		LogInfo(Channel::Scraping, std::format("OneTouch ({}): Menu survey - {} page(s) reached, {} expected-absent, {} notable failure(s)",
			device_id, survey.PagesReached, survey.ExpectedAbsent.size(), survey.NotableFailures.size()));

		if (!survey.EquipmentPageReached)
		{
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): Menu survey - the Equipment ON/OFF page was not reached; the discovered equipment set may be incomplete", device_id));
		}

		for (const auto& notable : survey.NotableFailures)
		{
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): Menu survey - unexpected failure to reach '{}'", device_id, notable));
		}

		for (const auto& expected : survey.ExpectedAbsent)
		{
			LogDebug(Channel::Scraping, std::format("OneTouch ({}): Menu survey - expected-absent page skipped: {}", device_id, expected));
		}

		return survey;
	}

}
// namespace AqualinkAutomate::Devices::OneTouch
