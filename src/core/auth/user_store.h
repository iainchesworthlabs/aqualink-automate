#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/entitlement.h"
#include "auth/group.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// UserStore — local user accounts (docs/auth-redesign.md §6, §9).
	//
	// Persisted to <auth-state-dir>/users.json (schema-versioned, atomic,
	// owner-only).  Every mutation persists immediately.  Usernames are
	// unique case-insensitively (ASCII).  PasswordHash holds the libsodium
	// crypto_pwhash_str encoding (argon2id) — this store never sees a
	// plaintext password.
	//
	// TokenVersion is the revocation/propagation counter (D15): bump it on
	// password change / disable / entitlement-affecting change and every
	// outstanding access token for the user becomes stale within a request.
	//
	// LAST-ADMIN PROTECTION: mutations that would leave the system with no
	// enabled subject holding system.admin (via groups or direct grants) are
	// refused — an admin cannot lock everyone out in one click.
	//=========================================================================

	struct UserRecord
	{
		std::string Id{};                       // Stable UUID (audit/prefs key).
		std::string Username{};
		std::string PasswordHash{};             // argon2id (crypto_pwhash_str).
		std::vector<std::string> Groups{};
		EntitlementSet DirectEntitlements{};
		std::uint32_t TokenVersion{ 1 };
		bool Disabled{ false };
	};

	class UserStore
	{
	public:
		// Missing file -> empty store (first-run).  Unreadable/wrong-schema ->
		// throws (identity data must never be silently dropped).
		static UserStore Load(const std::filesystem::path& file);

	public:
		bool Empty() const noexcept { return m_Users.empty(); }
		std::size_t Size() const noexcept { return m_Users.size(); }
		const std::vector<UserRecord>& All() const noexcept { return m_Users; }

		std::optional<UserRecord> FindById(std::string_view id) const;
		std::optional<UserRecord> FindByUsername(std::string_view username) const;   // Case-insensitive.

		// Create a new user (generates the Id).  Fails (false + error) on a
		// duplicate username or empty username/hash.
		bool Create(UserRecord user, std::string& error);

		// Update an existing record by Id.  Enforces username uniqueness and
		// last-admin protection (a change that disables or de-admins the final
		// enabled admin is refused).  `registry` resolves group-derived
		// entitlements for the admin check.
		bool Update(const UserRecord& user, const GroupRegistry& registry, std::string& error);

		// Remove by Id, subject to last-admin protection.
		bool Remove(std::string_view id, const GroupRegistry& registry, std::string& error);

		// Bump the revocation counter (D15) and persist.  Returns the new
		// version (0 when the user does not exist).
		std::uint32_t BumpTokenVersion(std::string_view id);

		// True when at least one ENABLED user resolves system.admin via groups
		// or direct grants.
		bool HasEnabledAdmin(const GroupRegistry& registry) const;

	private:
		UserStore() = default;

		void Save() const;

		bool WouldLoseLastAdmin(const UserRecord* changed_or_removed, const UserRecord* replacement, const GroupRegistry& registry) const;

	private:
		std::filesystem::path m_File{};
		std::vector<UserRecord> m_Users{};
	};

}
// namespace AqualinkAutomate::Auth
