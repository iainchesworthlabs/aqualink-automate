#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_options.h"
#include "http/server/make_response.h"
#include "options/options_option_type.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	HTTP::Response WebRoute_Diagnostics_Options::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Options::OnRequest", std::source_location::current());

		if (Verbs::get == req.method())
		{
			return HandleGet(req);
		}

		return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET.", {{"allowed", "GET"}});
	}

	HTTP::Response WebRoute_Diagnostics_Options::HandleGet(const HTTP::Request& req) const
	{
		nlohmann::json options = nlohmann::json::array();
		for (const auto& meta : Options::AppOption::RegisteredOptions())
		{
			nlohmann::json entry = nlohmann::json::object();
			entry["name"] = meta.long_name;
			entry["short_name"] = meta.short_name;
			entry["description"] = meta.description;
			options.push_back(entry);
		}

		nlohmann::json result;
		result["options"] = options;

		return MakeJsonResponse(req, HTTP::Status::ok, result.dump());
	}

}
// namespace AqualinkAutomate::HTTP
