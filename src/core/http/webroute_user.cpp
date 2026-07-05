#include <source_location>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_user.h"
#include "preferences/user_preferences_store.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_User::WebRoute_User(Auth::UserStore& users, Auth::GroupStore& groups, Auth::SessionService& session_service, Auth::SessionStore& sessions, Auth::AuditLog& audit, Preferences::UserPreferencesStore* user_prefs) :
		m_Users(users),
		m_Groups(groups),
		m_SessionService(session_service),
		m_Sessions(sessions),
		m_Audit(audit),
		m_UserPrefs(user_prefs)
	{
	}

	HTTP::Response WebRoute_User::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_User::OnRequest", std::source_location::current());

		const auto user_id = AdminRoutePathParam(req);

		if (!user_id.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "User not found" } }.dump());
		}

		switch (req.method())
		{
		case boost::beast::http::verb::get:
			return User_GetHandler(req, *user_id);

		case boost::beast::http::verb::put:
			return User_PutHandler(req, *user_id);

		case boost::beast::http::verb::delete_:
			return User_DeleteHandler(req, *user_id);

		default:
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "GET, PUT or DELETE required" } }.dump());
		}
	}

	HTTP::Response WebRoute_User::User_GetHandler(const HTTP::Request& req, const std::string& user_id)
	{
		const auto user = m_Users.FindById(user_id);

		if (!user.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "User not found" } }.dump());
		}

		return MakeJsonResponse(req, HTTP::Status::ok, UserRecordToJson(*user).dump());
	}

	HTTP::Response WebRoute_User::User_PutHandler(const HTTP::Request& req, const std::string& user_id)
	{
		const auto current = m_Users.FindById(user_id);

		if (!current.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "User not found" } }.dump());
		}

		const auto body = nlohmann::json::parse(req.body(), nullptr, false);

		if (body.is_discarded() || !body.is_object())
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected a JSON object body" } }.dump());
		}

		Auth::UserRecord updated = *current;

		if (body.contains("username"))
		{
			if (!body["username"].is_string() || body["username"].get<std::string>().empty())
			{
				return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Username must be a non-empty string" } }.dump());
			}

			updated.Username = body["username"].get<std::string>();
		}

		if (body.contains("groups"))
		{
			if (!body["groups"].is_array())
			{
				return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected groups to be an array of group names" } }.dump());
			}

			std::vector<std::string> groups;

			for (const auto& entry : body["groups"])
			{
				if (!entry.is_string())
				{
					return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected groups to be an array of group names" } }.dump());
				}

				groups.push_back(entry.get<std::string>());
			}

			updated.Groups = std::move(groups);
		}

		if (body.contains("direct_entitlements"))
		{
			std::string error;

			if (auto parsed = ParseEntitlementsField(body["direct_entitlements"], error); !parsed.has_value())
			{
				return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", error } }.dump());
			}
			else
			{
				updated.DirectEntitlements = std::move(*parsed);
			}
		}

		bool disable_requested = false;

		if (body.contains("disabled"))
		{
			if (!body["disabled"].is_boolean())
			{
				return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected disabled to be a boolean" } }.dump());
			}

			if (body["disabled"].get<bool>())
			{
				// Applied AFTER the field update, through SessionService::
				// DisableUser, so disable = tokver bump + every session revoked.
				disable_requested = !current->Disabled;
			}
			else
			{
				updated.Disabled = false;   // Re-enable is a plain field update.
			}
		}

		const bool entitlement_change = (updated.Groups != current->Groups) || (updated.DirectEntitlements != current->DirectEntitlements);

		if (std::string error; !m_Users.Update(updated, m_Groups.Registry(), error))
		{
			// Duplicate username / last-admin protection -> 409.
			return MakeJsonResponse(req, StatusForStoreError(error), nlohmann::json{ { "error", error } }.dump());
		}

		if (entitlement_change)
		{
			// D15 immediate propagation, tokver bump ONLY: outstanding access
			// tokens go stale within a request, while the still-valid refresh
			// tokens re-mint access tokens carrying the NEW entitlement set —
			// a grant change never forces a re-login.
			m_Users.BumpTokenVersion(user_id);
		}

		if (disable_requested)
		{
			if (std::string disable_error; !m_SessionService.DisableUser(user_id, disable_error))
			{
				// The field changes above have applied; the disable alone was
				// refused (last-admin protection) -> 409 with the store's error.
				return MakeJsonResponse(req, StatusForStoreError(disable_error), nlohmann::json{ { "error", disable_error } }.dump());
			}
		}

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.user_updated", "user", user_id, Routing::CurrentPeerIp()));

		const auto fresh = m_Users.FindById(user_id);

		return MakeJsonResponse(req, HTTP::Status::ok, (fresh.has_value() ? UserRecordToJson(*fresh) : UserRecordToJson(updated)).dump());
	}

	HTTP::Response WebRoute_User::User_DeleteHandler(const HTTP::Request& req, const std::string& user_id)
	{
		if (std::string error; !m_Users.Remove(user_id, m_Groups.Registry(), error))
		{
			// Unknown id -> 404; last-admin protection -> 409.
			return MakeJsonResponse(req, StatusForStoreError(error), nlohmann::json{ { "error", error } }.dump());
		}

		// Deletion semantics (§6): refresh sessions revoked and per-user
		// preference overrides forgotten; audit retained, keyed by the (now
		// former) user id.
		m_Sessions.RevokeAllForUser(user_id);

		if (nullptr != m_UserPrefs)
		{
			m_UserPrefs->Forget(user_id);
		}

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.user_deleted", "user", user_id, Routing::CurrentPeerIp()));

		HTTP::Response resp{ HTTP::Status::no_content, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.keep_alive(req.keep_alive());
		resp.prepare_payload();
		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
