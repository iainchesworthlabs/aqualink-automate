#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/session_store.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char SESSION_ROUTE_URL[] = "/api/sessions/{session_id}";

	// Per-session revoke ("log out that browser" — docs/auth-redesign.md §5,
	// D16): DELETE terminates one refresh session.  An admin (system.admin)
	// may revoke ANY session; every other authenticated subject only their
	// OWN — a non-owned (or unknown) id answers 404 either way, so the route
	// leaks no session-id existence.  Audited.
	//
	// RequiredAccess is PREFS_SELF (held implicitly by every authenticated
	// subject); the admin/owner distinction is enforced IN-HANDLER.
	class WebRoute_Session : public Interfaces::IWebRoute<SESSION_ROUTE_URL>
	{
	public:
		WebRoute_Session(Auth::SessionStore& sessions, Auth::AuditLog& audit);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::PREFS_SELF };
		}

	private:
		Auth::SessionStore& m_Sessions;
		Auth::AuditLog& m_Audit;
	};

}
// namespace AqualinkAutomate::HTTP
