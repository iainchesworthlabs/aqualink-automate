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
	inline constexpr char EQUIPMENT_SETPOINTS_ROUTE_URL[] = "/api/equipment/setpoints";

	class WebRoute_Equipment_Setpoints : public Interfaces::IWebRoute<EQUIPMENT_SETPOINTS_ROUTE_URL>
	{
	public:
		WebRoute_Equipment_Setpoints(Kernel::HubLocator& hub_locator);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_SETPOINTS };
		}

	private:
		HTTP::Response HandleGet(const HTTP::Request& req);
		HTTP::Response HandlePost(const HTTP::Request& req);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
