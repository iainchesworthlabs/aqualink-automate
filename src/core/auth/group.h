#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/entitlement.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// Group — a named entitlement set used as the assignment convenience in
	// the person -> group -> entitlements flow (docs/auth-redesign.md §4).
	//
	// Groups are NEVER a decision input: the PolicyEngine only sees the
	// flattened effective entitlement set.  Built-in groups exist from first
	// boot and cannot be deleted (their entitlements CAN be edited — that is
	// how an admin scopes Guest access):
	//
	//   - Everyone        entitlements every subject (incl. anonymous) holds
	//   - Guest           what the anonymous subject may do when auth is ON
	//   - Administrators  holds system.admin (superuser)
	//=========================================================================

	struct Group
	{
		std::string Name{};
		EntitlementSet Entitlements{};
		bool BuiltIn{ false };
	};

	namespace BuiltInGroups
	{
		inline constexpr std::string_view EVERYONE{ "Everyone" };
		inline constexpr std::string_view GUEST{ "Guest" };
		inline constexpr std::string_view ADMINISTRATORS{ "Administrators" };
	}

	//=========================================================================
	// GroupRegistry — the in-memory group collection (persistence arrives with
	// the stores in Slice 2; Slice 1 seeds the built-ins).  Lookup is by name;
	// unknown groups resolve to no entitlements (memberships referencing a
	// deleted group degrade safely instead of failing resolution).
	//=========================================================================
	class GroupRegistry
	{
	public:
		// A registry pre-seeded with the built-in groups (Everyone and Guest
		// start EMPTY — deny-by-default; Administrators holds system.admin).
		static GroupRegistry WithBuiltIns();

	public:
		void Upsert(Group group);
		std::optional<Group> Find(std::string_view name) const;
		const std::vector<Group>& All() const noexcept { return m_Groups; }

		// Flatten `direct` grants plus the entitlements of every group in
		// `memberships` (union semantics, unknown groups skipped) into the
		// subject's effective entitlement set.
		EntitlementSet ResolveEffectiveEntitlements(const EntitlementSet& direct, const std::vector<std::string>& memberships) const;

	private:
		std::vector<Group> m_Groups;
	};

}
// namespace AqualinkAutomate::Auth
