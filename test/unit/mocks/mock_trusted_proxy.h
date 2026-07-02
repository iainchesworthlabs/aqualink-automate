#pragma once

#include <string>
#include <vector>

#include "http/server/server_types.h"

namespace AqualinkAutomate::Test
{

	//=========================================================================
	// MockTrustedProxy — stamp forward-auth headers onto an HTTP::Request the
	// way a trusted reverse proxy (Authelia / authentik / oauth2-proxy) would
	// (docs/auth-redesign.md §12, "Mock trusted-proxy").
	//
	// Deliberately trivial: it exists so the Slice 5 forward-auth tests share
	// ONE canonical way to fake a proxy and header names never drift by test.
	//=========================================================================

	inline void ApplyForwardAuth(
		HTTP::Request& req,
		const std::string& user,
		const std::vector<std::string>& groups = {},
		const std::string& header = "Remote-User",
		const std::string& groups_header = "Remote-Groups")
	{
		req.set(header, user);

		if (!groups.empty())
		{
			// Comma-separated, matching the common proxy convention
			// (e.g. Authelia's Remote-Groups).
			std::string joined;

			for (const auto& group : groups)
			{
				if (!joined.empty())
				{
					joined += ',';
				}

				joined += group;
			}

			req.set(groups_header, joined);
		}
	}

}
// namespace AqualinkAutomate::Test
