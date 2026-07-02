#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_logout.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_AuthLogout::WebRoute_AuthLogout(Auth::SessionService& sessions) :
		m_Sessions(sessions)
	{
	}

	HTTP::Response WebRoute_AuthLogout::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthLogout::OnRequest", std::source_location::current());

		const auto make_response = [&](HTTP::Status status, std::string body)
		{
			HTTP::Response resp{ status, req.version() };
			resp.set(boost::beast::http::field::server, ServerFields::Server());
			resp.keep_alive(req.keep_alive());

			if (!body.empty())
			{
				resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
				resp.body() = std::move(body);
			}

			resp.prepare_payload();
			return resp;
		};

		if (boost::beast::http::verb::post != req.method())
		{
			return make_response(HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "POST required" } }.dump());
		}

		const auto body = nlohmann::json::parse(req.body(), nullptr, false);

		if (body.is_discarded() || !body.contains("refresh_token") || !body["refresh_token"].is_string())
		{
			return make_response(HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected JSON body with refresh_token" } }.dump());
		}

		const auto peer_ip = Routing::CurrentPeerIp();

		if (body.value("everywhere", false))
		{
			// Everywhere needs an authenticated subject — the refresh token
			// alone must not be able to nuke every other device's session.
			const auto& subject = Routing::CurrentSubject();

			if (!subject.Authenticated)
			{
				return make_response(HTTP::Status::unauthorized, nlohmann::json{ { "error", "Authentication required for everywhere logout" } }.dump());
			}

			m_Sessions.LogoutAll(subject.Id, peer_ip);
			return make_response(HTTP::Status::no_content, {});
		}

		m_Sessions.Logout(body["refresh_token"].get<std::string>(), peer_ip);

		// 204 regardless: logout must not disclose whether the token was live.
		return make_response(HTTP::Status::no_content, {});
	}

}
// namespace AqualinkAutomate::HTTP
