#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_apikey.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_ApiKey::WebRoute_ApiKey(Auth::ApiKeyStore& keys, Auth::AuditLog& audit) :
		m_Keys(keys),
		m_Audit(audit)
	{
	}

	HTTP::Response WebRoute_ApiKey::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_ApiKey::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::delete_ != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "DELETE required" } }.dump());
		}

		const auto key_id = AdminRoutePathParam(req);

		if (!key_id.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", "API key not found" } }.dump());
		}

		if (std::string error; !m_Keys.Revoke(*key_id, error))
		{
			return MakeJsonResponse(req, HTTP::Status::not_found, nlohmann::json{ { "error", error } }.dump());
		}

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.apikey_revoked", "apikey", *key_id, Routing::CurrentPeerIp()));

		HTTP::Response resp{ HTTP::Status::no_content, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.keep_alive(req.keep_alive());
		resp.prepare_payload();
		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
