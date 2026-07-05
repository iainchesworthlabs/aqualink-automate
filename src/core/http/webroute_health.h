#pragma once

#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char HEALTH_ROUTE_URL[] = "/api/health";

	// Unauthenticated liveness probe for container/orchestrator health checks
	// (Docker HEALTHCHECK, Kubernetes liveness, load balancers).
	//
	// Always answers 200 with a tiny, non-sensitive JSON body. Because requests are
	// processed on the single cooperative poll loop that also drives the protocol
	// stack, a hung event loop means this never responds -> the probe times out and
	// the orchestrator restarts the container. That is precisely the failure a
	// liveness probe should catch, so a plain 200 here is the correct semantics.
	//
	// Deliberately auth-EXEMPT (RequiresAuthentication() -> false) so the probe is
	// reachable without the operator's --api-auth-token. The body intentionally
	// omits version/build details so nothing sensitive leaks to an unauthenticated
	// caller (that information lives behind the gated /api/version).
	class WebRoute_Health : public Interfaces::IWebRoute<HEALTH_ROUTE_URL>
	{
	public:
		WebRoute_Health() = default;
		~WebRoute_Health() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;
		bool RequiresAuthentication() const final { return false; }
	};

}
// namespace AqualinkAutomate::HTTP
