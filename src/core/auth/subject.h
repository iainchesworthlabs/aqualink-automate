#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "auth/entitlement.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// Subject — the resolved identity + attributes attached to every request
	// by the subject-resolution middleware (docs/auth-redesign.md §4).
	//
	// A subject is produced from exactly one credential source; the PDP only
	// ever consumes the ATTRIBUTES (entitlements et al.), never the provider
	// or group names, so all providers converge on the same decision inputs.
	//=========================================================================

	enum class SubjectProvider
	{
		Anonymous,   // No credential presented (Guest group when auth is ON).
		Local,       // Local username/password session (Slice 2).
		ApiKey,      // API key / legacy bootstrap token.
		Oidc,        // In-app OIDC client (Slice 4).
		Proxy,       // Trusted reverse-proxy forward-auth (Slice 5).
		KioskPin     // Kiosk PIN elevation (Slice 3).
	};

	struct Subject
	{
		// Stable identity ("anonymous" when unauthenticated; a user id, API-key
		// id, or provider-scoped id otherwise).
		std::string Id{ "anonymous" };

		// Human-readable name for display surfaces: the local account's
		// username, an API key's label.  Empty when the provider has no natural
		// name (anonymous, kiosk) — clients fall back to Id.  Like groups,
		// NEVER a decision input.
		std::string Username{};

		// True only when a valid credential was presented and verified.
		bool Authenticated{ false };

		SubjectProvider Provider{ SubjectProvider::Anonymous };

		// Group memberships (assignment convenience; NEVER a decision input).
		std::vector<std::string> Groups{};

		// Effective entitlements = union(direct, group-derived, provider-derived);
		// the only subject attribute the PolicyEngine consults besides posture.
		EntitlementSet Entitlements{};

		// Token version at issuance (revocation/propagation check; 0 = n/a).
		std::uint32_t TokenVersion{ 0 };

		static Subject Anonymous()
		{
			return Subject{};
		}
	};

}
// namespace AqualinkAutomate::Auth
