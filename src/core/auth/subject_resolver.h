#pragma once

#include <chrono>
#include <memory>

#include "auth/api_key_store.h"
#include "auth/group.h"
#include "auth/jwt_codec.h"
#include "auth/kiosk_store.h"
#include "auth/user_store.h"
#include "http/server/routing/routing.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// MakeSubjectResolver — the subject-resolution middleware installed into
	// the routing layer via Routing::SetSubjectResolver() when --auth-mode is
	// enabled (docs/auth-redesign.md §4-§5).
	//
	// A presented bearer credential resolves in order:
	//
	//   1. a JWT minted by our JwtCodec — verified, then CROSS-CHECKED against
	//      the user store (D15 immediate revocation): a missing/disabled user
	//      or a TokenVersion behind the store's means the token is stale and
	//      the request degrades to anonymous.  Entitlements come from the
	//      `ent` claim, or are re-resolved from groups when the size-overflow
	//      rule elided them;
	//
	//   2. an API key (aak_..., or the folded-in legacy --api-auth-token) —
	//      matched by digest in the ApiKeyStore (revocation + expiry checked,
	//      last-used stamped), yielding a machine subject carrying the key's
	//      entitlements;
	//
	//   3. nothing / unverifiable -> the ANONYMOUS subject with the Guest
	//      group's (plus Everyone's) entitlements — deny-by-default.
	//
	// Null Users/ApiKeys are permitted (Slice-1 substrate tests): the
	// corresponding step is skipped.  The registry handle is LIVE (see
	// GroupStore::SharedRegistry) so admin edits apply to the next request.
	//=========================================================================

	struct SubjectResolverDeps
	{
		std::shared_ptr<GroupRegistry> Groups{};
		std::shared_ptr<JwtCodec> Codec{};
		std::shared_ptr<UserStore> Users{};       // Null => no tokver/disabled cross-check.
		std::shared_ptr<ApiKeyStore> ApiKeys{};   // Null => no machine credentials.
		std::shared_ptr<KioskStore> Kiosk{};      // Null => kiosk-PIN tokens rejected.
		JwtCodec::NowFn Now{ []() { return std::chrono::system_clock::now(); } };
	};

	HTTP::Routing::SubjectResolver MakeSubjectResolver(SubjectResolverDeps deps);

	// Slice-1 form (JWT + groups only); wraps the full resolver.
	HTTP::Routing::SubjectResolver MakeSubjectResolver(std::shared_ptr<GroupRegistry> groups, std::shared_ptr<JwtCodec> codec);

}
// namespace AqualinkAutomate::Auth
