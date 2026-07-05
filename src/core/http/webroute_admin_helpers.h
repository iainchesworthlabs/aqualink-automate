#pragma once

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <boost/url/parse.hpp>
#include <nlohmann/json.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/user_store.h"
#include "http/server/server_types.h"

// Shared helpers for the admin/user-management web routes (users, groups, API
// keys, sessions).  These collapse the boilerplate every admin route repeats
// (path-parameter extraction, entitlement-list validation, user-record
// serialisation, store-error -> status mapping and audit-event assembly) into
// one place so each route only carries its own behaviour.
namespace AqualinkAutomate::HTTP
{
	// Extract the path parameter from an admin route target.  Every admin item
	// route carries its single parameter as the THIRD path segment
	// ("/api/users/{user_id}", "/api/users/{user_id}/password",
	// "/api/groups/{group_name}", ...), so the extraction is uniform: re-parse
	// req.target() as an origin-form URL (the router matched on the same
	// segments) and return segment index 2, decoded.
	inline std::optional<std::string> AdminRoutePathParam(const HTTP::Request& req)
	{
		const auto url_result = boost::urls::parse_origin_form(req.target());

		if (!url_result.has_value())
		{
			return std::nullopt;
		}

		const auto segments = url_result->segments();

		std::size_t index = 0;
		for (const auto& segment : segments)
		{
			if ((2 == index) && !segment.empty())
			{
				return std::string{ segment };
			}

			++index;
		}

		return std::nullopt;
	}

	// Validate a JSON array of entitlement strings: every entry must parse as
	// an Entitlement (see entitlement.h) AND carry an action from the known
	// vocabulary (a typo must not silently create a dead grant).  On failure
	// `error` lists every rejected entry so the admin UI can show them all.
	inline std::optional<Auth::EntitlementSet> ParseEntitlementsField(const nlohmann::json& field, std::string& error)
	{
		if (!field.is_array())
		{
			error = "Expected an array of entitlement strings";
			return std::nullopt;
		}

		Auth::EntitlementSet entitlements;
		std::string rejected;

		for (const auto& entry : field)
		{
			if (!entry.is_string())
			{
				error = "Expected an array of entitlement strings";
				return std::nullopt;
			}

			const auto text = entry.get<std::string>();
			const auto parsed = Auth::Entitlement::Parse(text);

			if (!parsed.has_value() || !Auth::Vocabulary::IsKnownAction(parsed->Action()))
			{
				rejected += (rejected.empty() ? "" : ", ") + text;
				continue;
			}

			entitlements.Add(*parsed);
		}

		if (!rejected.empty())
		{
			error = std::format("Unknown or malformed entitlements: {}", rejected);
			return std::nullopt;
		}

		return entitlements;
	}

	// The public JSON shape of a user record: NEVER the password hash.
	inline nlohmann::json UserRecordToJson(const Auth::UserRecord& user)
	{
		return
		{
			{ "id", user.Id },
			{ "username", user.Username },
			{ "groups", user.Groups },
			{ "direct_entitlements", user.DirectEntitlements.ToStrings() },
			{ "disabled", user.Disabled },
			{ "tokver", user.TokenVersion }
		};
	}

	// Map a UserStore/GroupStore mutation error to the HTTP status the admin
	// API answers with: unknown target -> 404, conflicts (duplicate username,
	// last-admin protection, undeletable built-in) -> 409, anything else 400.
	inline HTTP::Status StatusForStoreError(std::string_view error)
	{
		using enum HTTP::Status;

		if (("User not found" == error) || ("Group not found" == error))
		{
			return not_found;
		}

		if (("Username already exists" == error) || ("Built-in groups cannot be removed" == error) || error.starts_with("Refused"))
		{
			return conflict;
		}

		return bad_request;
	}

	// Assemble the audit record for an admin action: who (the acting subject,
	// captured by the route), did what (auth event name), to which resource.
	inline Auth::AuditEvent MakeAdminAuditEvent(std::string_view actor_id, std::string_view action, std::string_view resource_kind, std::string_view resource_id, std::string_view peer_ip, std::string_view detail = {})
	{
		Auth::AuditEvent event;
		event.SubjectId = std::string{ actor_id };
		event.Provider = "Local";
		event.Action = std::string{ action };
		event.ResourceKind = std::string{ resource_kind };
		event.ResourceId = std::string{ resource_id };
		event.Decision = "success";
		event.PeerIp = std::string{ peer_ip };
		event.Detail = std::string{ detail };
		return event;
	}

}
// namespace AqualinkAutomate::HTTP
