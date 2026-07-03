#include <algorithm>
#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_groups.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		nlohmann::json GroupToJson(const Auth::Group& group)
		{
			return
			{
				{ "name", group.Name },
				{ "entitlements", group.Entitlements.ToStrings() },
				{ "built_in", group.BuiltIn }
			};
		}
	}
	// anonymous namespace

	WebRoute_Groups::WebRoute_Groups(Auth::GroupStore& groups, Auth::UserStore& users, Auth::AuditLog& audit) :
		m_Groups(groups),
		m_Users(users),
		m_Audit(audit)
	{
	}

	HTTP::Response WebRoute_Groups::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Groups::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::get == req.method())
		{
			nlohmann::json groups = nlohmann::json::array();

			for (const auto& group : m_Groups.Registry().All())
			{
				groups.push_back(GroupToJson(group));
			}

			return MakeJsonResponse(req, HTTP::Status::ok, groups.dump());
		}

		if (boost::beast::http::verb::post != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "GET or POST required" } }.dump());
		}

		const auto body = nlohmann::json::parse(req.body(), nullptr, false);

		if (body.is_discarded() || !body.contains("name") || !body["name"].is_string() || !body.contains("entitlements"))
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected JSON body with name and entitlements" } }.dump());
		}

		const auto name = body["name"].get<std::string>();

		std::string error;
		auto entitlements = ParseEntitlementsField(body["entitlements"], error);

		if (!entitlements.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", error } }.dump());
		}

		// Snapshot BEFORE the upsert: propagation below needs to know whether
		// the effective entitlements actually changed.
		const auto previous = m_Groups.Registry().Find(name);
		const bool entitlement_change = !previous.has_value() || (previous->Entitlements != *entitlements);

		if (!m_Groups.Upsert(Auth::Group{ .Name = name, .Entitlements = *entitlements, .BuiltIn = false }, error))
		{
			return MakeJsonResponse(req, StatusForStoreError(error), nlohmann::json{ { "error", error } }.dump());
		}

		if (entitlement_change)
		{
			// D15 immediate propagation: every member's outstanding access
			// tokens must go stale so the next refresh re-mints with the
			// group's new entitlement set.  (A brand-new NAME can still have
			// members: user records referencing a previously-deleted group
			// degrade safely and spring back to life on re-creation.)
			for (const auto& user : m_Users.All())
			{
				if (std::ranges::contains(user.Groups, name))
				{
					m_Users.BumpTokenVersion(user.Id);
				}
			}
		}

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.group_upserted", "group", name, Routing::CurrentPeerIp()));

		const auto fresh = m_Groups.Registry().Find(name);

		return MakeJsonResponse(req, HTTP::Status::ok, (fresh.has_value() ? GroupToJson(*fresh) : nlohmann::json{ { "name", name } }).dump());
	}

}
// namespace AqualinkAutomate::HTTP
