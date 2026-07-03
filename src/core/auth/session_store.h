#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// SessionStore — refresh-token sessions (docs/auth-redesign.md §5, D2).
	//
	// One record per login session, persisted to <auth-state-dir>/
	// sessions.json (schema-versioned, atomic, owner-only) so sessions
	// survive a restart.  The refresh SECRET ("art_<random>") is returned
	// once at creation/rotation and only its SHA-256 digest is stored.
	//
	// ROTATION + REUSE DETECTION: every Rotate() replaces the secret and
	// remembers the previous digest.  A presented secret matching a session's
	// PREVIOUS digest means a rotated-out token was replayed — the classic
	// stolen-refresh-token signature — so the whole session is revoked and
	// the caller told to treat it as an incident (audit).
	//=========================================================================

	struct SessionRecord
	{
		std::string Id{};
		std::string UserId{};
		std::string RefreshDigest{};      // Current secret's SHA-256 (hex).
		std::string PreviousDigest{};     // Rotated-out secret (reuse tripwire).
		std::int64_t IssuedUnix{ 0 };
		std::int64_t ExpiryUnix{ 0 };     // Absolute session lifetime.
		std::int64_t LastSeenUnix{ 0 };
		std::string UserAgent{};
		std::string PeerIp{};
	};

	class SessionStore
	{
	public:
		static SessionStore Load(const std::filesystem::path& file);

	public:
		const std::vector<SessionRecord>& All() const noexcept { return m_Sessions; }
		std::vector<SessionRecord> ForUser(std::string_view user_id) const;

		// Open a session; returns the one-time refresh secret ("art_...").
		std::string Create(std::string user_id, std::int64_t expiry_unix, std::string user_agent, std::string peer_ip, std::int64_t now_unix, std::string& out_session_id);

		struct RotateResult
		{
			bool Success{ false };
			bool ReuseDetected{ false };  // Rotated-out token replayed -> session revoked.
			std::string NewRefreshSecret{};
			std::string SessionId{};
			std::string UserId{};
		};

		// Exchange a presented refresh secret for a fresh one (single use).
		// Expired sessions fail (and are pruned); a PREVIOUS-digest match
		// revokes the session and reports ReuseDetected.
		RotateResult Rotate(std::string_view presented_secret, std::int64_t now_unix);

		// Terminate by presented secret (logout), by id (admin/self session
		// management), or all sessions for a user (logout-everywhere/disable).
		bool RevokeByRefresh(std::string_view presented_secret);
		bool RevokeById(std::string_view session_id);
		std::size_t RevokeAllForUser(std::string_view user_id);

		void PruneExpired(std::int64_t now_unix);

	private:
		SessionStore() = default;

		void Save() const;

		static std::string GenerateSecret();

	private:
		std::filesystem::path m_File{};
		std::vector<SessionRecord> m_Sessions{};
	};

}
// namespace AqualinkAutomate::Auth
