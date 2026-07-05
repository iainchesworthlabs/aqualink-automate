#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char HEALTH_DETAILED_ROUTE_URL[] = "/api/health/detailed";

	// Richer health / readiness report for operators and monitoring. It complements
	// the bare liveness probe at /api/health (see WebRoute_Health) in two ways:
	//
	//   * It is GATED by the normal security policy (RequiresAuthentication() stays
	//     true). The body exposes internal subsystem state — detected board model,
	//     equipment-validation anomalies, device counts, MQTT connectivity — which
	//     should not be world-readable once an --api-auth-token is configured. In the
	//     default (no-auth) deployment it is freely reachable.
	//
	//   * Its status code reflects READINESS, not just liveness: 200 once the system
	//     has determined its pool configuration (the same gate the command routes
	//     use), 503 while still starting / unconfigured. That makes it usable as a
	//     Kubernetes readiness probe, distinct from /api/health's liveness role.
	//
	// Serial/protocol counters are intentionally NOT duplicated here — those live in
	// the Prometheus exposition at /metrics. This view focuses on readiness,
	// configuration, equipment, and integration connectivity.
	class WebRoute_HealthDetailed : public Interfaces::IWebRoute<HEALTH_DETAILED_ROUTE_URL>
	{
	public:
		explicit WebRoute_HealthDetailed(Kernel::HubLocator& hub_locator);
		~WebRoute_HealthDetailed() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
		}

	private:
		// Some hubs (notably the MQTT integration) are registered AFTER the routes
		// are constructed, so resolve every hub lazily per-request rather than
		// caching a possibly-null handle at construction.
		Kernel::HubLocator& m_HubLocator;
	};

}
// namespace AqualinkAutomate::HTTP
