#pragma once

#include <functional>
#include <utility>

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
		// setup_required (optional): true while first-run setup is still open
		// (auth-mode enabled + zero users) so the SPA can route straight to the
		// setup wizard instead of the login form.
		// kiosk_enabled (optional): true when kiosk PIN elevation is configured,
		// so the login screen can offer PIN entry alongside the password form.
		explicit WebRoute_AuthMe(std::function<bool()> setup_required = {}, std::function<bool()> kiosk_enabled = {}) :
			m_SetupRequired(std::move(setup_required)),
			m_KioskEnabled(std::move(kiosk_enabled))
		{
		}

		~WebRoute_AuthMe() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		std::function<bool()> m_SetupRequired;
		std::function<bool()> m_KioskEnabled;
	};

}
// namespace AqualinkAutomate::HTTP
