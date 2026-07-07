#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "kernel/orp.h"
#include "kernel/ph.h"
#include "kernel/hub_events/data_hub_config_event.h"
#include "types/units_dimensionless.h"

namespace AqualinkAutomate::Kernel
{

	class DataHub_ConfigEvent_Chemistry : public DataHub_ConfigEvent
	{
	public:
		DataHub_ConfigEvent_Chemistry();
		~DataHub_ConfigEvent_Chemistry() override = default;

		std::optional<Kernel::ORP> ORP() const;
		std::optional<Kernel::pH> pH() const;
		std::optional<Units::ppm_quantity> SaltLevel() const;

		void ORP(const Kernel::ORP& orp);
		void pH(const Kernel::pH& pH);
		void SaltLevel(const Units::ppm_quantity& salt_level_in_ppm);

		nlohmann::json ToJSON() const override;

	private:
		std::optional<Kernel::ORP> m_ORP{ std::nullopt };
		std::optional<Kernel::pH> m_pH{ std::nullopt };
		std::optional<Units::ppm_quantity> m_SaltLevel{ std::nullopt };
	};

}
// namespace namespace AqualinkAutomate::Kernel
