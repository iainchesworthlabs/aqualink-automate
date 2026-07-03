#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/webroute_entitlements.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	HTTP::Response WebRoute_Entitlements::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Entitlements::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::get != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "GET required" } }.dump());
		}

		nlohmann::json actions = nlohmann::json::array();

		for (const auto action : Auth::Vocabulary::ALL_ACTIONS)
		{
			actions.push_back(std::string{ action });
		}

		return MakeJsonResponse(req, HTTP::Status::ok, nlohmann::json{ { "actions", actions } }.dump());
	}

}
// namespace AqualinkAutomate::HTTP
