#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// Shared persistence for the auth stores (users/groups/api-keys —
	// docs/auth-redesign.md §9): schema-versioned JSON documents, written
	// atomically (temp-then-rename) and locked to owner-only, mirroring the
	// JwtKeyStore/preferences pattern.
	//
	// LoadAuthStoreFile:
	//   - missing file            -> std::nullopt (caller starts empty/seeded)
	//   - unreadable / bad schema -> throws std::runtime_error (identity data
	//     must never be silently dropped or regenerated)
	//=========================================================================

	inline constexpr std::uint32_t AUTH_STORE_SCHEMA_VERSION{ 1 };

	std::optional<nlohmann::json> LoadAuthStoreFile(const std::filesystem::path& file);

	void SaveAuthStoreFile(const std::filesystem::path& file, nlohmann::json document);

	// SHA-256 hex digest — the shared at-rest form for high-entropy secrets
	// (API keys, refresh tokens).  NOT for passwords (argon2id via
	// PasswordHasher); these secrets are random 256-bit values, so a fast
	// hash is the correct primitive.
	std::string Sha256Hex(std::string_view data);

}
// namespace AqualinkAutomate::Auth
