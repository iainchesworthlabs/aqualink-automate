#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/kiosk_service.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char KIOSK_ROUTE_URL[] = "/api/kiosk";

	// Admin surface for kiosk PIN elevation (docs/auth-redesign.md §6, D16),
	// all methods gated on system.admin:
	//
	//   GET    -> { enabled, target_group }         (NEVER the PIN or its hash)
	//   PUT    { pin, target_group } -> 204         (set/replace the PIN)
	//   DELETE -> 204                               (disable + clear)
	//
	// DEFERRED-RESPONSE route: PUT hashes the PIN with argon2id on the
	// OffloadPool, so — like login — the whole route is async; GET/DELETE
	// simply complete synchronously inside OnRequestAsync.
	class WebRoute_Kiosk : public Interfaces::IWebRoute<KIOSK_ROUTE_URL>
	{
	public:
		WebRoute_Kiosk(Auth::KioskService& kiosk, boost::asio::any_io_executor executor);

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

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
