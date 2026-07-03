#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/password_hasher.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char USERS_ROUTE_URL[] = "/api/users";

	// User collection (docs/auth-redesign.md §6): GET lists every account
	// (id, username, groups, direct entitlements, disabled, tokver — NEVER
	// password hashes); POST { username, password, groups?,
	// direct_entitlements? } creates one (201).  Weak password / unknown
	// entitlement -> 400; duplicate username -> 409.
	//
	// DEFERRED-RESPONSE route: the new password's argon2id hash runs on the
	// OffloadPool; the duplicate-username check re-runs in the completion (the
	// kernel-thread serialisation point), so two racing creates cannot both
	// claim a username.
	class WebRoute_Users : public Interfaces::IWebRoute<USERS_ROUTE_URL>
	{
	public:
		WebRoute_Users(Auth::UserStore& users, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor);

	public:
		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override;

		// Unreached (IsAsyncRoute); satisfies the pure-virtual.
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		Auth::UserStore& m_Users;
		Auth::AuditLog& m_Audit;
		Utility::OffloadPool& m_Offload;
		Auth::PasswordHasher::Params m_HashParams;
		boost::asio::any_io_executor m_Executor;
	};

}
// namespace AqualinkAutomate::HTTP
