#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

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

}
// namespace AqualinkAutomate::Auth
