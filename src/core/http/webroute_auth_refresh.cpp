#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_refresh.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_AuthRefresh::WebRoute_AuthRefresh(Auth::SessionService& sessions) :
		m_Sessions(sessions)
	{
	}

	HTTP::Response WebRoute_AuthRefresh::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthRefresh::OnRequest", std::source_location::current());

		const auto make_response = [&](HTTP::Status status, const nlohmann::json& body)
		{
			HTTP::Response resp{ status, req.version() };
			resp.set(boost::beast::http::field::server, ServerFields::Server());
			resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
			resp.keep_alive(req.keep_alive());
			resp.body() = body.dump();
			resp.prepare_payload();
			return resp;
		};

		if (boost::beast::http::verb::post != req.method())
		{
			return make_response(HTTP::Status::method_not_allowed, { { "error", "POST required" } });
		}

		const auto body = nlohmann::json::parse(req.body(), nullptr, false);

		if (body.is_discarded() || !body.contains("refresh_token") || !body["refresh_token"].is_string())
		{
			return make_response(HTTP::Status::bad_request, { { "error", "Expected JSON body with refresh_token" } });
		}

		const auto result = m_Sessions.Refresh(body["refresh_token"].get<std::string>(), Routing::CurrentPeerIp());

		if (!result.Success)
		{
			return make_response(HTTP::Status::unauthorized, { { "error", result.Error } });
		}

		return make_response(HTTP::Status::ok,
			{
				{ "access_token", result.AccessToken },
				{ "refresh_token", result.RefreshToken },
				{ "token_type", "Bearer" }
			});
	}

}
// namespace AqualinkAutomate::HTTP
