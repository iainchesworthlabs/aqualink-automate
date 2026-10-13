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

		// The salt-water generator's state (the same object the REST chemistry payload carries
		// under "chlorinator"). Present only on an event that is reporting a chlorinator change;
		// a JSON null is a real value meaning "the chlorinator is gone", distinct from "absent".
		std::optional<nlohmann::json> Chlorinator() const;
		void Chlorinator(const nlohmann::json& chlorinator);

		nlohmann::json ToJSON() const override;

	private:
		std::optional<Kernel::ORP> m_ORP{ std::nullopt };
		std::optional<Kernel::pH> m_pH{ std::nullopt };
		std::optional<Units::ppm_quantity> m_SaltLevel{ std::nullopt };
		std::optional<nlohmann::json> m_Chlorinator{ std::nullopt };
	};

}
// namespace namespace AqualinkAutomate::Kernel
