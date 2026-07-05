#include <algorithm>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_group.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Group::WebRoute_Group(Auth::GroupStore& groups, Auth::UserStore& users, Auth::AuditLog& audit) :
		m_Groups(groups),
		m_Users(users),
		m_Audit(audit)
	{
	}

	HTTP::Response WebRoute_Group::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Group::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::delete_ != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "DELETE required" } }.dump());
		}

		const auto name = AdminRoutePathParam(req);

		if (!name.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "Group not found" } }.dump());
		}

		if (std::string error; !m_Groups.Remove(*name, error))
		{
			// Built-in -> 409; unknown -> 404.
			return MakeJsonResponse(req, StatusForStoreError(error), nlohmann::json{ { "error", error } }.dump());
		}

		// D15 immediate propagation: the members just LOST this group's
		// entitlements, so their outstanding access tokens must go stale.
		for (const auto& user : m_Users.All())
		{
			if (std::ranges::contains(user.Groups, *name))
			{
				m_Users.BumpTokenVersion(user.Id);
			}
		}

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.group_deleted", "group", *name, Routing::CurrentPeerIp()));

		HTTP::Response resp{ HTTP::Status::no_content, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.keep_alive(req.keep_alive());
		resp.prepare_payload();
		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
