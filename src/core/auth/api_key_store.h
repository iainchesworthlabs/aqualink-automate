#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/entitlement.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// ApiKeyStore — machine credentials for the HTTP API (docs/auth-redesign
	// §7, §9): <auth-state-dir>/api-keys.json, schema-versioned, atomic,
	// owner-only.
	//
	// The key SECRET ("aak_<random>") is returned exactly once at creation
	// and only its SHA-256 digest is persisted; presented keys are matched by
	// digest.  (API keys are high-entropy random values, so a fast hash — not
	// argon2 — is the correct primitive; the comparison is by fixed-length
	// digest lookup.)  Keys carry their entitlements directly, an optional
	// expiry, and a last-used timestamp for hygiene audits.
	//
	// The legacy --api-auth-token folds in as a pre-seeded bootstrap key when
	// auth-mode is enabled (SeedBootstrapKey) so existing deployments keep
	// working with their configured secret.
	//=========================================================================

	struct ApiKeyRecord
	{
		std::string Id{};
		std::string Label{};
		std::string SecretSha256Hex{};
		EntitlementSet Entitlements{};
		std::int64_t ExpiryUnix{ 0 };      // 0 == never expires.
		std::int64_t LastUsedUnix{ 0 };    // 0 == never used.
		bool Revoked{ false };
	};

	class ApiKeyStore
	{
	public:
		static ApiKeyStore Load(const std::filesystem::path& file);

		// SHA-256 hex digest of a presented secret (exposed for tests).
		static std::string DigestOf(std::string_view secret);

	public:
		const std::vector<ApiKeyRecord>& All() const noexcept { return m_Keys; }
		std::optional<ApiKeyRecord> FindById(std::string_view id) const;

		// Create a key with the given label + entitlements; returns the
		// one-time secret ("aak_..." — NEVER persisted or logged).
		std::string Create(std::string label, EntitlementSet entitlements, std::int64_t expiry_unix, std::string& out_key_id);

		// Idempotently seed the legacy --api-auth-token as the bootstrap key
		// (system.admin scope, no expiry) so it keeps working under auth-mode.
		void SeedBootstrapKey(std::string_view legacy_token);

		// Resolve a presented secret to its ACTIVE record (not revoked, not
		// expired at `now_unix`), stamping last-used.  nullopt otherwise.
		std::optional<ApiKeyRecord> Authenticate(std::string_view presented_secret, std::int64_t now_unix);

		bool Revoke(std::string_view id, std::string& error);

	private:
		ApiKeyStore() = default;

		void Save() const;

	private:
		std::filesystem::path m_File{};
		std::vector<ApiKeyRecord> m_Keys{};
	};

}
// namespace AqualinkAutomate::Auth
