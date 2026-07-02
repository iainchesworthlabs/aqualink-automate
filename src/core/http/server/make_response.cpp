#include <utility>

#include "http/server/make_response.h"
#include "http/server/server_fields.h"

namespace AqualinkAutomate::HTTP
{

	Response MakeResponse(const Request& req, Status code, std::string_view content_type, std::string body)
	{
		Response resp{ code, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, content_type);
		resp.keep_alive(req.keep_alive());
		resp.body() = std::move(body);
		resp.prepare_payload();
		return resp;
	}

	Response MakeJsonResponse(const Request& req, Status code, std::string body)
	{
		return MakeResponse(req, code, ContentTypes::APPLICATION_JSON, std::move(body));
	}

	Response MakeErrorResponse(const Request& req, Status code, std::string_view error_code, std::string_view message, const nlohmann::json& params)
	{
		nlohmann::json body{
			{ "error", message },
			{ "code", error_code }
		};

		if (params.is_object() && !params.empty())
		{
			body["params"] = params;
		}

		return MakeJsonResponse(req, code, body.dump());
	}

}
// namespace AqualinkAutomate::HTTP
