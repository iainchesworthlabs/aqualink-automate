#pragma once

#include <boost/asio/any_io_executor.hpp>

#include "auth/kiosk_service.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_PIN_ROUTE_URL[] = "/api/auth/pin";

	// POST { "pin": ... } -> 200 { access_token, refresh_token, session_id } |
	// 401 (wrong PIN / kiosk not enabled — one indistinguishable error) | 429
	// (kiosk PIN lockout) | 400 (malformed).
	//
	// Kiosk PIN elevation (docs/auth-redesign.md §6, D16): a short PIN elevates
	// the anonymous Guest into the admin-configured target group.  Like the
	// password login this is a DEFERRED-RESPONSE route — the argon2id verify
	// runs on the OffloadPool so the RS-485 loop never blocks.
	//
	// Declares NO RequiredAccess: the PIN is how an anonymous visitor ACQUIRES
	// entitlements.  The routing rate limiter still applies per-IP and the
	// KioskService adds its own lockout.
	class WebRoute_AuthPin : public Interfaces::IWebRoute<AUTH_PIN_ROUTE_URL>
	{
	public:
		WebRoute_AuthPin(Auth::KioskService& kiosk, boost::asio::any_io_executor executor);

	public:
		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override;

		// Unreached (IsAsyncRoute); satisfies the pure-virtual for completeness.
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		Auth::KioskService& m_Kiosk;
		boost::asio::any_io_executor m_Executor;
	};

}
// namespace AqualinkAutomate::HTTP
