#pragma once

#include <string>

#include <boost/uuid/uuid.hpp>
#include <nlohmann/json.hpp>

#include "kernel/hub_events/data_hub_config_event.h"

namespace AqualinkAutomate::Kernel
{

	class DataHub_ConfigEvent_ButtonStateChange : public DataHub_ConfigEvent
	{
	public:
		DataHub_ConfigEvent_ButtonStateChange(const boost::uuids::uuid& button_id, std::string_view status);
		DataHub_ConfigEvent_ButtonStateChange(const boost::uuids::uuid& button_id, std::string_view status, std::string_view label);
		~DataHub_ConfigEvent_ButtonStateChange() override = default;

		boost::uuids::uuid ButtonId() const;
		std::string_view Status() const;
		std::string_view Label() const;

		nlohmann::json ToJSON() const override;

	private:
		boost::uuids::uuid m_ButtonId;
		std::string m_Status;
		std::string m_Label;
	};

}
// namespace AqualinkAutomate::Kernel
