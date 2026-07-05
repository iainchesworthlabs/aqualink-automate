#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENT_HEATER_ROUTE_URL[] = "/api/equipment/heater";

	// POST enables/disables a heater, identified by its body of water. Body fields:
	//   { "body": "pool" | "spa" | "solar", "enable": true | false }
	// Maps to ICommandDispatcher::SetHeaterMode (pool/spa heater, or the solar heater via the
	// Shared body). 400 on a bad value, 503 when no commandable controller is present.
	class WebRoute_Equipment_Heater : public Interfaces::IWebRoute<EQUIPMENT_HEATER_ROUTE_URL>
	{
	public:
		explicit WebRoute_Equipment_Heater(Kernel::HubLocator& hub_locator);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_HEATER };
		}

	private:
		std::shared_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
