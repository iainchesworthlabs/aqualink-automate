#pragma once

#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_ME_ROUTE_URL[] = "/api/auth/me";

	// Self-describing identity probe: returns the resolved Subject for the
	// calling request (id, provider, groups, effective entitlements) plus the
	// global posture, so the SPA can gate its affordances (lock icons, hidden
	// admin nav) from one source of truth — with the server-side PolicyEngine
	// remaining the actual enforcement point.
	//
	// Deliberately declares NO RequiredAccess: an anonymous/guest caller must
	// be able to ask "who am I and what may I do?" (the answer being the Guest
	// scope) — the same shape the login screen uses to decide what to offer.
	class WebRoute_AuthMe : public Interfaces::IWebRoute<AUTH_ME_ROUTE_URL>
	{
	public:
		WebRoute_AuthMe() = default;
		~WebRoute_AuthMe() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;
	};

}
// namespace AqualinkAutomate::HTTP
