#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/group_store.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Preferences { class UserPreferencesStore; }

namespace AqualinkAutomate::HTTP
{
	inline constexpr char USER_ROUTE_URL[] = "/api/users/{user_id}";

	// Individual user (docs/auth-redesign.md §6): GET one (404 unknown);
	// PUT { groups?, direct_entitlements?, disabled?, username? } updates it;
	// DELETE removes it (refresh sessions revoked, audit entries retained).
	//
	// Propagation (D15): a groups/direct-entitlements change bumps the user's
	// TokenVersion ONLY — outstanding access tokens go stale immediately, and
	// the still-valid refresh tokens re-mint with the new entitlement set (no
	// forced re-login for a grant change).  disabled=true instead routes
	// through SessionService::DisableUser, which revokes every session as well.
	// Last-admin protection (duplicate username too) surfaces as 409 with the
	// store's error.
	class WebRoute_User : public Interfaces::IWebRoute<USER_ROUTE_URL>
	{
	public:
		// user_prefs (nullable): when present, a deleted user's per-user
		// preference overrides are forgotten too (auth-mode on).
		WebRoute_User(Auth::UserStore& users, Auth::GroupStore& groups, Auth::SessionService& session_service, Auth::SessionStore& sessions, Auth::AuditLog& audit, Preferences::UserPreferencesStore* user_prefs = nullptr);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		HTTP::Response User_GetHandler(const HTTP::Request& req, const std::string& user_id);
		HTTP::Response User_PutHandler(const HTTP::Request& req, const std::string& user_id);
		HTTP::Response User_DeleteHandler(const HTTP::Request& req, const std::string& user_id);

	private:
		Auth::UserStore& m_Users;
		Auth::GroupStore& m_Groups;
		Auth::SessionService& m_SessionService;
		Auth::SessionStore& m_Sessions;
		Auth::AuditLog& m_Audit;
		Preferences::UserPreferencesStore* m_UserPrefs;
	};

}
// namespace AqualinkAutomate::HTTP
