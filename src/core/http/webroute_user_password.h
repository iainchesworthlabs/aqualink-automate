#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/group_store.h"
#include "auth/password_hasher.h"
#include "auth/session_store.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char USER_PASSWORD_ROUTE_URL[] = "/api/users/{user_id}/password";

	// Password lifecycle (docs/auth-redesign.md §6): PUT { password } sets the
	// user's password.  An admin (system.admin) may set ANYONE's; every other
	// authenticated subject may only set their OWN (403 otherwise).  On success
	// the user's TokenVersion is bumped AND every refresh session is revoked —
	// a password change invalidates everything — and the change is audited
	// ("auth.password_changed").
	//
	// RequiredAccess is PREFS_SELF (held implicitly by every authenticated
	// subject), so the router gate only means "authenticated"; the admin/self
	// distinction above is enforced IN-HANDLER by comparing the path user_id
	// to Routing::CurrentSubject().Id in the synchronous prefix.
	//
	// DEFERRED-RESPONSE route: the argon2id hash of the new password runs on
	// the OffloadPool.
	class WebRoute_UserPassword : public Interfaces::IWebRoute<USER_PASSWORD_ROUTE_URL>
	{
	public:
		WebRoute_UserPassword(Auth::UserStore& users, Auth::GroupStore& groups, Auth::SessionStore& sessions, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor);

		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override;

		// Unreached (IsAsyncRoute); satisfies the pure-virtual.
		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::PREFS_SELF };
		}

	private:
		Auth::UserStore& m_Users;
		Auth::GroupStore& m_Groups;
		Auth::SessionStore& m_Sessions;
		Auth::AuditLog& m_Audit;
		Utility::OffloadPool& m_Offload;
		Auth::PasswordHasher::Params m_HashParams;
		boost::asio::any_io_executor m_Executor;
	};

}
// namespace AqualinkAutomate::HTTP
