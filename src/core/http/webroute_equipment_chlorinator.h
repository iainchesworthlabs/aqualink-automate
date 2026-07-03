#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENT_CHLORINATOR_ROUTE_URL[] = "/api/equipment/chlorinator";

	// POST sets the salt-water-generator output. Body fields (both optional):
	//   { "percentage": 0..100 }   -> SetChlorinatorPercentage
	//   { "boost": true|false }    -> SetChlorinatorBoost
	// 400 on a bad value, 503 when no commandable chlorinator/dispatcher is present.
	class WebRoute_Equipment_Chlorinator : public Interfaces::IWebRoute<EQUIPMENT_CHLORINATOR_ROUTE_URL>
	{
	public:
		explicit WebRoute_Equipment_Chlorinator(Kernel::HubLocator& hub_locator);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_CHLORINATOR };
		}

	private:
		HTTP::Response HandlePost(const HTTP::Request& req);

	private:
		std::shared_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
