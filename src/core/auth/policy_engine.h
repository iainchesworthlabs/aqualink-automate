#pragma once

#include <string>
#include <string_view>

#include "auth/subject.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// PolicyEngine — the ABAC policy decision point (docs/auth-redesign.md §4).
	//
	//     Decide(subject, action, resource, environment) -> Permit | Deny
	//
	// Default DENY.  A request is permitted when:
	//
	//   1. the posture is auth-OFF (environment.AuthEnabled == false): the
	//      whole identity system is disabled and every request is the
	//      historical "root-anonymous" — Permit; or
	//   2. the subject holds the system.admin superuser entitlement; or
	//   3. the subject holds an entitlement whose action matches exactly and
	//      whose resource-selector matches the resource id (see entitlement.h
	//      for selector semantics).
	//
	// Decisions are attribute-driven only: the engine never inspects group or
	// provider names.  v1 environment = posture; the struct is the extension
	// point for later conditions (time-of-day, source network, ...).
	//=========================================================================

	enum class Decision
	{
		Permit,
		Deny
	};

	// The resource half of an access request.  Kind names the route-declared
	// resource category ("aux", "schedule", ...; empty when the action has no
	// per-resource grain); Id is the specific instance the request targets
	// (empty when none — e.g. a collection read).
	struct ResourceRef
	{
		std::string Kind{};
		std::string Id{};
	};

	struct Environment
	{
		// Global posture: false == auth disabled (root-anonymous permit-all).
		bool AuthEnabled{ false };
	};

	class PolicyEngine
	{
	public:
		static Decision Decide(const Subject& subject, std::string_view action, const ResourceRef& resource, const Environment& environment);
	};

}
// namespace AqualinkAutomate::Auth
