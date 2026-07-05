#pragma once

#include "auth/session_service.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_REFRESH_ROUTE_URL[] = "/api/auth/refresh";

	// POST { "refresh_token": ... } -> 200 { access_token, refresh_token } |
	// 401.  Rotation is single-use: the presented token dies, a new one comes
	// back, and replaying a rotated-out token revokes the whole session
	// (stolen-token tripwire, audited).  Synchronous: only digest lookups.
	//
	// No RequiredAccess: the refresh token IS the credential.
	class WebRoute_AuthRefresh : public Interfaces::IWebRoute<AUTH_REFRESH_ROUTE_URL>
	{
	public:
		explicit WebRoute_AuthRefresh(Auth::SessionService& sessions);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		Auth::SessionService& m_Sessions;
	};

}
// namespace AqualinkAutomate::HTTP
