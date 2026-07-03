#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/session_store.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char SESSIONS_ROUTE_URL[] = "/api/sessions";

	// Active-session list (docs/auth-redesign.md §5, D16): GET returns the
	// refresh-token sessions — id, user id, issued/expiry/last-seen, user
	// agent and peer address (never any token material).
	//
	// RequiredAccess is PREFS_SELF (held implicitly by every authenticated
	// subject), so the router gate only means "authenticated"; the scope is
	// then widened IN-HANDLER: a subject holding system.admin sees EVERY
	// session, anyone else sees only their OWN (so no peer-ip redaction
	// question arises — a non-admin never sees another user's rows).
	class WebRoute_Sessions : public Interfaces::IWebRoute<SESSIONS_ROUTE_URL>
	{
	public:
		explicit WebRoute_Sessions(Auth::SessionStore& sessions);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::PREFS_SELF };
		}

	private:
		Auth::SessionStore& m_Sessions;
	};

}
// namespace AqualinkAutomate::HTTP
