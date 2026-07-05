#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// Entitlement — one structured authorization scope of the form
	//
	//     <domain>.<action>[:<resource-selector>]
	//
	// e.g. "equipment.view", "equipment.control.aux:*",
	//      "equipment.control.aux:5e17c9b2-...".
	//
	// The dotted action path is matched EXACTLY (no hierarchy is implied); only
	// the optional selector carries wildcard semantics:
	//
	//   - selector absent  -> matches requests that carry NO resource id
	//   - selector "*"     -> matches ANY resource id (including none)
	//   - selector "<id>"  -> matches exactly that resource id
	//
	// Entitlements are the primary subject attribute consumed by the
	// PolicyEngine (see policy_engine.h); people acquire them via groups (plus
	// optional direct grants) — see group.h.
	//=========================================================================
	class Entitlement
	{
	public:
		explicit Entitlement(std::string action, std::optional<std::string> selector = std::nullopt);

		// Parse "<domain>.<action>[:<selector>]".  The action path requires at
		// least two dot-separated segments of [a-z0-9_-]; the selector (after the
		// first ':') is taken verbatim and must be non-empty when present.
		// Returns std::nullopt on malformed input.
		static std::optional<Entitlement> Parse(std::string_view text);

		const std::string& Action() const noexcept { return m_Action; }
		const std::optional<std::string>& Selector() const noexcept { return m_Selector; }

		// True when this entitlement satisfies a request for `action` against the
		// resource identified by `resource_id` (empty when the action targets no
		// specific resource).  See the selector rules above.
		bool Matches(std::string_view action, std::string_view resource_id = {}) const;

		std::string ToString() const;

		bool operator==(const Entitlement& other) const = default;

	private:
		std::string m_Action;
		std::optional<std::string> m_Selector;
	};

	//=========================================================================
	// EntitlementSet — a deduplicated collection of entitlements with the
	// aggregate Permits() query used by the PolicyEngine, and a stable string
	// form used for the JWT `ent` claim (sorted so token contents are
	// deterministic and directly assertable in tests).
	//=========================================================================
	class EntitlementSet
	{
	public:
		EntitlementSet() = default;

		// Parse a list of textual entitlements, skipping malformed entries.  When
		// `rejected` is provided the malformed inputs are reported through it so
		// callers (stores, admin APIs) can surface them.
		static EntitlementSet Parse(const std::vector<std::string>& texts, std::vector<std::string>* rejected = nullptr);

		void Add(Entitlement entitlement);
		void Merge(const EntitlementSet& other);

		bool Contains(const Entitlement& entitlement) const;

		// True when any member entitlement matches the (action, resource) pair.
		bool Permits(std::string_view action, std::string_view resource_id = {}) const;

		// Sorted textual form (deterministic; feeds the JWT `ent` claim).
		std::vector<std::string> ToStrings() const;

		std::size_t Size() const noexcept { return m_Entitlements.size(); }
		bool Empty() const noexcept { return m_Entitlements.empty(); }

		bool operator==(const EntitlementSet& other) const = default;

	private:
		std::vector<Entitlement> m_Entitlements;
	};

}
// namespace AqualinkAutomate::Auth
