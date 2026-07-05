#include <algorithm>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_session.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Session::WebRoute_Session(Auth::SessionStore& sessions, Auth::AuditLog& audit) :
		m_Sessions(sessions),
		m_Audit(audit)
	{
	}

	HTTP::Response WebRoute_Session::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Session::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::delete_ != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "DELETE required" } }.dump());
		}

		const auto not_found = [&]()
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "Session not found" } }.dump());
		};

		const auto session_id = AdminRoutePathParam(req);

		if (!session_id.has_value())
		{
			return not_found();
		}

		const auto& subject = Routing::CurrentSubject();
		const bool is_admin = subject.Entitlements.Permits(Auth::Vocabulary::SYSTEM_ADMIN);

		const auto& all = m_Sessions.All();
		// Unknown id and someone ELSE's id answer identically (404): a
		// non-admin cannot probe which session ids exist.
		if (const auto it = std::ranges::find_if(all, [&](const auto& session) { return session.Id == *session_id; }); (all.end() == it) || (!is_admin && (it->UserId != subject.Id)))
		{
			return not_found();
		}

		m_Sessions.RevokeById(*session_id);

		m_Audit.Record(MakeAdminAuditEvent(subject.Id, "auth.session_revoked", "session", *session_id, Routing::CurrentPeerIp()));

		HTTP::Response resp{ HTTP::Status::no_content, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.keep_alive(req.keep_alive());
		resp.prepare_payload();
		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
