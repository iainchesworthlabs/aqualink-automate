#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iequipmentdiscoverycontroller.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::EquipmentCache
{
	class EquipmentCacheService;
}
// namespace AqualinkAutomate::EquipmentCache

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_AUXREDISCOVERY_ROUTE_URL[] = "/api/diagnostics/aux-rediscovery";

	// Diagnostics "clear & rediscover" action: wipes every auto-detected auxillary (sparing any
	// forced Present by an operator override) and starts a fresh full-discovery crawl. GET
	// reports status; POST triggers it. Destructive by nature, so POST is SYSTEM_ADMIN-gated
	// (stricter than DIAGNOSTICS_VIEW for the read side), same split as WebRoute_Diagnostics_Recording.
	class WebRoute_Diagnostics_AuxRediscovery : public Interfaces::IWebRoute<DIAGNOSTICS_AUXREDISCOVERY_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_AuxRediscovery(Kernel::HubLocator& hub_locator, std::shared_ptr<EquipmentCache::EquipmentCacheService> equipment_cache_service);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		HTTP::Response HandleGet(const HTTP::Request& req) const;
		HTTP::Response HandlePost(const HTTP::Request& req) const;

	private:
		// Non-owning: the controller is owned by the OneTouchDevice for its lifetime.
		// nullptr when no OneTouch controller is registered (e.g. IAQ-only/RSSA-only rigs, or
		// dev-mode/replay before one attaches), in which case the route reports
		// in_progress=false / last_cleared_count=0 and rejects a POST with 503.
		std::shared_ptr<Interfaces::IEquipmentDiscoveryController> m_DiscoveryController;

		// Flushed (best-effort) right after a successful clear, so the on-disk cache doesn't
		// resurrect the just-cleared phantoms if the app restarts before the next periodic save.
		// Optional: null when running with an empty --equipment-cache-file.
		std::shared_ptr<EquipmentCache::EquipmentCacheService> m_EquipmentCacheService;
	};

}
// namespace AqualinkAutomate::HTTP
