#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char ENTITLEMENTS_ROUTE_URL[] = "/api/entitlements";

	// GET the entitlement action vocabulary (docs/auth-redesign.md §4) so the
	// admin UI can enumerate the assignable actions (the selector grammar —
	// "action[:selector]" — is a client-side concern).  Read-only.
	class WebRoute_Entitlements : public Interfaces::IWebRoute<ENTITLEMENTS_ROUTE_URL>
	{
	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}
	};

}
// namespace AqualinkAutomate::HTTP
