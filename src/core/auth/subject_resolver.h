#pragma once

#include <memory>

#include "auth/group.h"
#include "auth/jwt_codec.h"
#include "http/server/routing/routing.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// MakeSubjectResolver — the subject-resolution middleware installed into
	// the routing layer via Routing::SetSubjectResolver() when --auth-mode is
	// enabled (docs/auth-redesign.md §4).
	//
	// Slice 1 resolves two credential shapes:
	//
	//   - a Bearer JWT minted by our own JwtCodec (issuance arrives with the
	//     login flows in Slice 2; tests mint directly) -> the token's subject,
	//     with entitlements taken from the `ent` claim, or re-resolved from
	//     the group registry when the size-overflow rule elided them; and
	//
	//   - nothing (or anything unverifiable) -> the ANONYMOUS subject, whose
	//     entitlements are the Guest group's (plus Everyone's) — deny-by-
	//     default until an admin grants guest scope.
	//
	// Later slices extend this same seam: API keys + legacy-token fold-in
	// (Slice 2), kiosk PIN sessions (Slice 3), external OIDC tokens (Slice 4),
	// trusted-proxy forwarded headers (Slice 5).
	//=========================================================================
	HTTP::Routing::SubjectResolver MakeSubjectResolver(std::shared_ptr<GroupRegistry> groups, std::shared_ptr<JwtCodec> codec);

}
// namespace AqualinkAutomate::Auth
