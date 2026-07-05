#pragma once

#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_CHECK_ROUTE_URL[] = "/api/auth/check";

	// Lightweight probe the UI uses to discover its authentication state.
	// Because the routing layer enforces the bearer policy BEFORE dispatching a
	// registered route, this handler is reached only when the request is already
	// authorised (or when auth is disabled entirely), so it always answers 200.
	// An unauthenticated request to a token-protected server is rejected upstream
	// with 401 and never reaches here.
	class WebRoute_AuthCheck : public Interfaces::IWebRoute<AUTH_CHECK_ROUTE_URL>
	{
	public:
		WebRoute_AuthCheck() = default;
		~WebRoute_AuthCheck() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;
	};

}
// namespace AqualinkAutomate::HTTP
