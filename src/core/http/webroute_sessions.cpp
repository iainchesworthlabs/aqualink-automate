#include <source_location>

#include <nlohmann/json.hpp>

#include "auth/entitlement_vocabulary.h"
#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/webroute_sessions.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// The public JSON shape of a session record: NEVER the refresh digests.
		nlohmann::json SessionToJson(const Auth::SessionRecord& session)
		{
			return
			{
				{ "id", session.Id },
				{ "user_id", session.UserId },
				{ "issued_unix", session.IssuedUnix },
				{ "expiry_unix", session.ExpiryUnix },
				{ "last_seen_unix", session.LastSeenUnix },
				{ "user_agent", session.UserAgent },
				{ "peer_ip", session.PeerIp }
			};
		}
	}
	// anonymous namespace

	WebRoute_Sessions::WebRoute_Sessions(Auth::SessionStore& sessions) :
		m_Sessions(sessions)
	{
	}

	HTTP::Response WebRoute_Sessions::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Sessions::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::get != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "GET required" } }.dump());
		}

		// Admin widening (see the class comment): system.admin sees every
		// session, everyone else only their own.
		const auto& subject = Routing::CurrentSubject();
		const bool is_admin = subject.Entitlements.Permits(Auth::Vocabulary::SYSTEM_ADMIN);

		nlohmann::json sessions = nlohmann::json::array();

		if (is_admin)
		{
			for (const auto& session : m_Sessions.All())
			{
				sessions.push_back(SessionToJson(session));
			}
		}
		else
		{
			for (const auto& session : m_Sessions.ForUser(subject.Id))
			{
				sessions.push_back(SessionToJson(session));
			}
		}

		return MakeJsonResponse(req, HTTP::Status::ok, sessions.dump());
	}

}
// namespace AqualinkAutomate::HTTP
