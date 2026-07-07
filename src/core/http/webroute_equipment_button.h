#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/iwebroute.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENTBUTTONS_BUTTON_ROUTE_URL[] = "/api/equipment/buttons/{button_id}";

	class WebRoute_Equipment_Button: public Interfaces::IWebRoute<EQUIPMENTBUTTONS_BUTTON_ROUTE_URL>
	{
	public:
		explicit WebRoute_Equipment_Button(Kernel::HubLocator& hub_locator);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_AUX, .ResourceKind = "aux" };
		}

		HTTP::Response ButtonIndividual_GetHandler(const HTTP::Request& req);
		HTTP::Response ButtonIndividual_PostHandler(const HTTP::Request& req);

	private:
		HTTP::Response ButtonToggle_MapResultToResponse(const HTTP::Request& req, const std::string& button_id, const std::shared_ptr<Kernel::AuxillaryDevice>& button_device, Interfaces::ICommandDispatcher::CommandResult result);

		HTTP::Response Report_ButtonDoesntExist(const HTTP::Request& req, const std::string& button_id);
		HTTP::Response Report_SystemIsInactive(const HTTP::Request& req);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
