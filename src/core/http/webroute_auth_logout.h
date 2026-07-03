#pragma once

#include "auth/session_service.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_LOGOUT_ROUTE_URL[] = "/api/auth/logout";

	// POST { "refresh_token": ... [, "everywhere": true] } -> 204.
	//
	// Plain logout drops the presented session.  "everywhere" additionally
	// requires an AUTHENTICATED subject and revokes every session for that
	// subject + bumps tokver, so outstanding access tokens die immediately
	// (D15).  Always answers 204 for a well-formed request — logout must not
	// leak whether a token was live.
	class WebRoute_AuthLogout : public Interfaces::IWebRoute<AUTH_LOGOUT_ROUTE_URL>
	{
	public:
		explicit WebRoute_AuthLogout(Auth::SessionService& sessions);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		Auth::SessionService& m_Sessions;
	};

}
// namespace AqualinkAutomate::HTTP
