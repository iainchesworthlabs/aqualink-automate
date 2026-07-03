#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>

#include "auth/audit_log.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/kiosk_store.h"
#include "auth/password_hasher.h"
#include "auth/session_store.h"
#include "utility/offload_pool.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// KioskService — kiosk PIN elevation (docs/auth-redesign.md §6, D16).
	//
	// A wall-tablet quick-elevation: a short admin-configured PIN elevates the
	// anonymous Guest into a designated group.  The design deliberately reuses
	// the local-session machinery:
	//
	//   - the PIN is verified with argon2id on the OffloadPool (the kernel loop
	//     never blocks) with a decoy hash so response timing does not reveal
	//     whether a PIN is even configured;
	//   - success mints an ordinary access JWT (provider = KioskPin) carrying
	//     the target group's entitlements, plus a rotating refresh token in the
	//     SessionStore — so a kiosk session is revocable and appears in the
	//     session list like any other;
	//   - a single in-memory failure bucket locks the PIN endpoint after too
	//     many attempts (the per-IP limiter in the routing layer still applies);
	//   - kiosk sessions are NOT user records: the subject resolver validates
	//     them against KioskStore (enabled + TokenVersion) instead of the user
	//     store, and grants NO prefs.self (a shared terminal has no "self").
	//
	// Every outcome is recorded on the audit trail.
	//=========================================================================
	class KioskService
	{
	public:
		// The synthetic subject id every kiosk session shares (sessions are
		// listed / revoked under it; it is never a UserStore record).
		static constexpr std::string_view SubjectId{ "kiosk" };

		struct Config
		{
			std::chrono::seconds RefreshTtl{ std::chrono::hours{ 24 * 14 } };
			unsigned MaxFailures{ 5 };
			std::chrono::seconds Lockout{ std::chrono::minutes{ 15 } };
			std::size_t MinPinLength{ 4 };
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

		struct SetPinResult
		{
			bool Success{ false };
			std::string Error{};
		};

		using LoginCompletion = std::function<void(LoginResult)>;
		using SetPinCompletion = std::function<void(SetPinResult)>;

	public:
		KioskService(std::shared_ptr<KioskStore> kiosk,
			std::shared_ptr<GroupStore> groups,
			std::shared_ptr<SessionStore> sessions,
			std::shared_ptr<JwtCodec> codec,
			Utility::OffloadPool& offload,
			AuditLog& audit,
			Config config);

	public:
		bool Enabled() const;
		std::string TargetGroup() const;

		// Admin: validate the PIN policy + target group, hash the PIN on the
		// OffloadPool, then enable elevation into that group.  Revokes any prior
		// kiosk sessions.  `completion` is posted to `executor`.
		void SetPin(std::string raw_pin, std::string target_group, std::string actor_id, std::string peer_ip, boost::asio::any_io_executor executor, SetPinCompletion completion);

		// Admin: disable elevation, clear the PIN, and revoke all live kiosk
		// sessions.  Synchronous (no crypto).
		void ClearPin(std::string_view actor_id, std::string_view peer_ip);

		// Public: verify a presented PIN on the OffloadPool and, on success,
		// mint a kiosk session.  `completion` is posted to `executor`.
		void LoginWithPin(std::string raw_pin, std::string peer_ip, std::string user_agent, boost::asio::any_io_executor executor, LoginCompletion completion);

	private:
		std::string MintKioskToken() const;

		void RecordFailure();
		bool IsLockedOut();

		std::int64_t NowUnix() const;

	private:
		std::shared_ptr<KioskStore> m_Kiosk;
		std::shared_ptr<GroupStore> m_Groups;
		std::shared_ptr<SessionStore> m_Sessions;
		std::shared_ptr<JwtCodec> m_Codec;
		Utility::OffloadPool& m_Offload;
		AuditLog& m_Audit;
		Config m_Config;

		std::string m_DecoyHash;   // Timing-equalising hash when no PIN is set.

		struct FailureEntry
		{
			unsigned Count{ 0 };
			std::chrono::system_clock::time_point LockedUntil{};
		};

		FailureEntry m_Failure;    // Single bucket (the PIN has no username).
	};

}
// namespace AqualinkAutomate::Auth
