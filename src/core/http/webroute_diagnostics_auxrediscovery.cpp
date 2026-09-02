#include "http/webroute_diagnostics_auxrediscovery.h"

#include <nlohmann/json.hpp>

#include "equipment_cache/equipment_cache_service.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		nlohmann::json StatusToJson(const Interfaces::IEquipmentDiscoveryController::DiscoveryStatusSnapshot& status)
		{
			nlohmann::json result;
			result["in_progress"] = status.in_progress;
			result["last_cleared_count"] = status.last_cleared_count;
			return result;
		}
	}
	// anonymous namespace

	// TryFind (not Find): the discovery controller is only present once a OneTouch controller has
	// attached. In dev-mode/replay (or an IAQ-only/RSSA-only rig) there is none, and the route
	// should still construct and report in_progress=false rather than throw.
	WebRoute_Diagnostics_AuxRediscovery::WebRoute_Diagnostics_AuxRediscovery(Kernel::HubLocator& hub_locator, std::shared_ptr<EquipmentCache::EquipmentCacheService> equipment_cache_service)
		: m_DiscoveryController(hub_locator.TryFind<Interfaces::IEquipmentDiscoveryController>())
		, m_EquipmentCacheService(std::move(equipment_cache_service))
	{
	}

	HTTP::Response WebRoute_Diagnostics_AuxRediscovery::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_AuxRediscovery::OnRequest", std::source_location::current());

		switch (req.method())
		{
		case Verbs::get:
			return HandleGet(req);

		case Verbs::post:
			return HandlePost(req);

		default:
			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET or POST.", {{"allowed", "GET, POST"}});
		}
	}

	HTTP::Response WebRoute_Diagnostics_AuxRediscovery::HandleGet(const HTTP::Request& req) const
	{
		Interfaces::IEquipmentDiscoveryController::DiscoveryStatusSnapshot status;
		if (m_DiscoveryController)
		{
			status = m_DiscoveryController->DiscoveryStatus();
		}
		// When no controller is registered the default-constructed status reports
		// in_progress=false / last_cleared_count=0, which is the correct picture.

		return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(status).dump());
	}

	HTTP::Response WebRoute_Diagnostics_AuxRediscovery::HandlePost(const HTTP::Request& req) const
	{
		if (!m_DiscoveryController)
		{
			LogWarning(Channel::Web, "Aux rediscovery requested but no discovery controller is available (no OneTouch controller attached?)");
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "aux_rediscovery_unavailable", "Rediscovery is not available in this mode");
		}

		if (!m_DiscoveryController->RequestFullRediscovery())
		{
			LogInfo(Channel::Web, "Aux rediscovery requested via web UI but refused (already in progress, or the controller is not actively emulating)");
			return MakeErrorResponse(req, HTTP::Status::conflict, "aux_rediscovery_busy", "A rediscovery crawl is already in progress, or the controller is not currently active");
		}

		LogInfo(Channel::Web, "Aux rediscovery started via web UI");

		if (m_EquipmentCacheService)
		{
			// Best-effort: flush now so the persisted cache doesn't resurrect the just-cleared
			// phantoms if the app restarts before the next periodic save fires.
			m_EquipmentCacheService->SaveNow();
		}

		return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(m_DiscoveryController->DiscoveryStatus()).dump());
	}

}
// namespace AqualinkAutomate::HTTP
