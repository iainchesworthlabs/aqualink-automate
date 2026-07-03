#pragma once

#include <boost/asio/any_io_executor.hpp>

#include "auth/session_service.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_LOGIN_ROUTE_URL[] = "/api/auth/login";

	// POST { "username": ..., "password": ... } -> 200 { access_token,
	// refresh_token, session_id } | 401 (one indistinguishable error for wrong
	// password / unknown user) | 429 (account lockout) | 400 (malformed).
	//
	// DEFERRED-RESPONSE route: the argon2id verify runs on the OffloadPool
	// (docs/auth-redesign.md §6) — the completion fires on `executor` (the
	// kernel io_context), so the RS-485 loop never blocks on a login.
	//
	// Deliberately declares NO RequiredAccess: login is how a subject ACQUIRES
	// entitlements.  The routing layer's rate limiter still applies per-IP,
	// and the SessionService adds per-ACCOUNT lockout.
	class WebRoute_AuthLogin : public Interfaces::IWebRoute<AUTH_LOGIN_ROUTE_URL>
	{
	public:
		WebRoute_AuthLogin(Auth::SessionService& sessions, boost::asio::any_io_executor executor);

	public:
		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override;

		// Unreached (IsAsyncRoute); satisfies the pure-virtual for completeness.
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		Auth::SessionService& m_Sessions;
		boost::asio::any_io_executor m_Executor;
	};

}
// namespace AqualinkAutomate::HTTP
