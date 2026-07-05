#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/asio/any_io_executor.hpp>

#include "auth/audit_log.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/password_hasher.h"
#include "auth/session_store.h"
#include "auth/user_store.h"
#include "utility/offload_pool.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// SessionService — the local login/logout/refresh flows (docs/auth-
	// redesign.md §5-§6, D2/D4/D15).
	//
	//   - Login verifies argon2id on the OffloadPool (the kernel thread is
	//     never blocked; the completion — and every store mutation — runs on
	//     the caller-supplied executor).  Unknown usernames verify against a
	//     pre-computed decoy hash so response timing does not enumerate
	//     accounts.
	//   - Success mints a short-lived access JWT (entitlements as claims) +
	//     an opaque rotating refresh token recorded in the SessionStore.
	//   - Refresh rotates single-use tokens; replay of a rotated-out token
	//     revokes the session (stolen-token tripwire).
	//   - Per-ACCOUNT lockout (in-memory): too many failures against one
	//     username locks that account's login for a window, independent of
	//     the per-IP limiter in the routing layer.
	//   - Revocation (D15): logout-all and DisableUser bump the user's
	//     TokenVersion so outstanding access tokens go stale immediately
	//     (the subject resolver cross-checks tokver on every request).
	//
	// Every outcome is recorded on the audit trail.
	//=========================================================================
	class SessionService
	{
	public:
		struct Config
		{
			std::chrono::seconds RefreshTtl{ std::chrono::hours{ 24 * 14 } };
			unsigned MaxFailuresPerAccount{ 5 };
			std::chrono::seconds AccountLockout{ std::chrono::minutes{ 15 } };
			PasswordHasher::Params HashParams{};   // Tests: PasswordHasher::TestParams().
			JwtCodec::NowFn Now{ []() { return std::chrono::system_clock::now(); } };
		};

		struct LoginResult
		{
			bool Success{ false };
			bool LockedOut{ false };
			std::string Error{};
			std::string AccessToken{};
			std::string RefreshToken{};
			std::string SessionId{};
		};

		struct RefreshResult
		{
			bool Success{ false };
			std::string Error{};
			std::string AccessToken{};
			std::string RefreshToken{};
		};

		using LoginCompletion = std::function<void(LoginResult)>;

	public:
		SessionService(std::shared_ptr<UserStore> users,
			std::shared_ptr<GroupStore> groups,
			std::shared_ptr<SessionStore> sessions,
			std::shared_ptr<JwtCodec> codec,
			Utility::OffloadPool& offload,
			AuditLog& audit,
			Config config);

		// Asynchronous: argon2 runs on the offload pool; `completion` is posted
		// to `executor` (the kernel io_context in production).
		void Login(std::string username, std::string password, std::string peer_ip, std::string user_agent, boost::asio::any_io_executor executor, LoginCompletion completion);

		RefreshResult Refresh(const std::string& refresh_token, std::string_view peer_ip);

		bool Logout(const std::string& refresh_token, std::string_view peer_ip);

		// Logout-everywhere: drops every session AND bumps tokver so live
		// access tokens die with them.  Returns the session count dropped.
		std::size_t LogoutAll(std::string_view user_id, std::string_view peer_ip);

		// Disable + full revocation in one step (admin action; D15).
		bool DisableUser(std::string_view user_id, std::string& error);

		// Mint an access token for a user record (exposed for the first-run/
		// bootstrap flow and tests; login uses it internally).
		std::string MintAccessToken(const UserRecord& user) const;

	private:
		void FinishLogin(bool verified, UserRecord user, std::string peer_ip, std::string user_agent, LoginCompletion& completion);

		void RecordFailure(const std::string& username_key);
		bool IsLockedOut(const std::string& username_key);

		std::int64_t NowUnix() const;

	private:
		std::shared_ptr<UserStore> m_Users;
		std::shared_ptr<GroupStore> m_Groups;
		std::shared_ptr<SessionStore> m_Sessions;
		std::shared_ptr<JwtCodec> m_Codec;
		Utility::OffloadPool& m_Offload;
		AuditLog& m_Audit;
		Config m_Config;

		std::string m_DecoyHash;   // Timing-equalising hash for unknown users.

		struct FailureEntry
		{
			unsigned Count{ 0 };
			std::chrono::system_clock::time_point LockedUntil{};
		};

		std::unordered_map<std::string, FailureEntry> m_Failures;
	};

}
// namespace AqualinkAutomate::Auth
