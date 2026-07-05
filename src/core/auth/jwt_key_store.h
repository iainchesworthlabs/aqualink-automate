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
	// JwtKeyStore — owns the HS256 signing secrets for internally-issued
	// session tokens (docs/auth-redesign.md §5).
	//
	// Keys live in a single JSON file inside the hardened auth state
	// directory (0600, owner-only — same posture as the TLS private key).
	// Every key carries a `kid` (hash-derived) which Sign() stamps into the
	// token header; Rotate() installs a fresh active key while keeping the
	// previous one valid so tokens issued just before rotation still verify
	// during their lifetime (the "grace" key).
	//
	// External/OIDC token validation (RS256 via JWKS) is a separate concern
	// and arrives with Slice 4 — this store is only for tokens WE mint.
	//=========================================================================

	struct SigningKey
	{
		std::string Kid{};
		std::vector<std::uint8_t> Secret{};
		std::int64_t CreatedUnix{ 0 };
	};

	class JwtKeyStore
	{
	public:
		// Load the key file, creating it (with a fresh random key) when absent.
		// Throws std::runtime_error when the file exists but cannot be parsed —
		// silently regenerating would invalidate every outstanding session.
		static JwtKeyStore LoadOrCreate(const std::filesystem::path& key_file);

		const SigningKey& Active() const;
		std::optional<SigningKey> Find(std::string_view kid) const;

		// Install a fresh active key; the previous active key is retained (and
		// older keys dropped) so recently-issued tokens verify through the
		// rotation.  Persists immediately.
		void Rotate();

		std::size_t KeyCount() const noexcept { return m_Keys.size(); }

	private:
		JwtKeyStore() = default;

		void Save() const;

		static SigningKey GenerateKey();

	private:
		std::filesystem::path m_KeyFile{};
		std::vector<SigningKey> m_Keys{};   // Front == active.
	};

}
// namespace AqualinkAutomate::Auth
