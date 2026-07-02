#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// KioskStore — kiosk PIN elevation config (docs/auth-redesign.md §6, D16):
	// <auth-state-dir>/kiosk.json, schema-versioned, atomic, owner-only.
	//
	// A single record: an enabled flag, the argon2id-hashed PIN, the target
	// group a successful PIN elevates an anonymous visitor into, and a
	// TokenVersion that bumps on every configuration change so that live kiosk
	// access tokens die at once — the subject resolver cross-checks it, exactly
	// as it does UserRecord::TokenVersion for local users (D15).
	//
	// The PIN is a LOW-entropy secret (a handful of digits), so it is hashed
	// with argon2id off-thread via PasswordHasher — never a fast hash.  This
	// store only persists the resulting self-describing hash; the hashing and
	// verification live in KioskService.
	//=========================================================================
	class KioskStore
	{
	public:
		static KioskStore Load(const std::filesystem::path& file);

	public:
		bool Enabled() const noexcept { return m_Enabled; }
		const std::string& TargetGroup() const noexcept { return m_TargetGroup; }
		const std::string& PinHash() const noexcept { return m_PinHash; }
		std::uint32_t TokenVersion() const noexcept { return m_TokenVersion; }

		// Enable kiosk PIN elevation with a pre-computed argon2id hash and the
		// target group.  Bumps TokenVersion (any prior kiosk access tokens go
		// stale immediately).  Persists.
		void Configure(std::string pin_hash, std::string target_group);

		// Disable kiosk PIN elevation and clear the hash + target group.  Bumps
		// TokenVersion.  Persists.
		void Disable();

	private:
		KioskStore() = default;

		void Save() const;

	private:
		std::filesystem::path m_File{};
		bool m_Enabled{ false };
		std::string m_PinHash{};
		std::string m_TargetGroup{};
		std::uint32_t m_TokenVersion{ 1 };
	};

}
// namespace AqualinkAutomate::Auth
