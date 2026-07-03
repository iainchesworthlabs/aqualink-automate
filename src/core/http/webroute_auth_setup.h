#pragma once

#include <boost/asio/any_io_executor.hpp>

#include "auth/audit_log.h"
#include "auth/password_hasher.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"
#include "utility/offload_pool.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char AUTH_SETUP_ROUTE_URL[] = "/api/auth/setup";

	// First-run setup: POST { "username": ..., "password": ... } while the
	// user store is EMPTY creates the first administrator (201; the UI then
	// logs in normally); once any user exists it answers 403 permanently —
	// self-sealing, so it is deliberately open (there is no one to
	// authenticate as yet).
	//
	// DEFERRED-RESPONSE route: the argon2id hash of the new password runs on
	// the OffloadPool; the emptiness check re-runs in the completion (the
	// kernel-thread serialisation point), so two racing setup attempts
	// cannot both create an admin.
	class WebRoute_AuthSetup : public Interfaces::IWebRoute<AUTH_SETUP_ROUTE_URL>
	{
	public:
		WebRoute_AuthSetup(Auth::UserStore& users, Auth::AuditLog& audit, Utility::OffloadPool& offload, Auth::PasswordHasher::Params hash_params, boost::asio::any_io_executor executor);

	public:
		bool IsAsyncRoute() const override { return true; }

		void OnRequestAsync(const HTTP::Request& req, AsyncCompletion complete) override;

		// Unreached (IsAsyncRoute); satisfies the pure-virtual.
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	private:
		Auth::UserStore& m_Users;
		Auth::AuditLog& m_Audit;
		Utility::OffloadPool& m_Offload;
		Auth::PasswordHasher::Params m_HashParams;
		boost::asio::any_io_executor m_Executor;
	};

}
// namespace AqualinkAutomate::HTTP
