#include <string>

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>
#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_logging.h"
#include "http/server/make_response.h"
#include "logging/logging.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	HTTP::Response WebRoute_Diagnostics_Logging::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Logging::OnRequest", std::source_location::current());

		switch (req.method())
		{
		case Verbs::get:
			return HandleGet(req);

		case Verbs::post:
			return HandlePost(req);

		default:
			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET or POST.", {{"allowed", "GET, POST"}});
		}
	}

	HTTP::Response WebRoute_Diagnostics_Logging::HandleGet(const HTTP::Request& req)
	{
		nlohmann::json result;

		// Build channel → current severity map
		nlohmann::json channels = nlohmann::json::object();
		magic_enum::enum_for_each<Channel>([&channels](auto const& channel_entry)
			{
				auto channel_severity = SeverityFiltering::GetChannelFilterLevel(channel_entry.value);
				channels[std::string(magic_enum::enum_name(channel_entry.value))] = std::string(magic_enum::enum_name(channel_severity));
			});

		result["channels"] = channels;

		// Build available severity levels array
		nlohmann::json levels = nlohmann::json::array();
		magic_enum::enum_for_each<Severity>([&levels](auto const& severity_entry)
			{
				levels.push_back(std::string(magic_enum::enum_name(severity_entry.value)));
			});

		result["severity_levels"] = levels;

		return MakeJsonResponse(req, HTTP::Status::ok, result.dump());
	}

	HTTP::Response WebRoute_Diagnostics_Logging::HandlePost(const HTTP::Request& req)
	{
		try
		{
			auto body = nlohmann::json::parse(req.body());

			if (body.contains("global"))
			{
				auto level_name = body["global"].get<std::string>();
				auto severity_cast = magic_enum::enum_cast<Severity>(level_name);

				if (!severity_cast.has_value())
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_severity", "Invalid severity level");
				}

				SeverityFiltering::SetGlobalFilterLevel(severity_cast.value());

				LogInfo(Channel::Web, std::format("Global log level set to {} via web UI", level_name));
			}
			else if (body.contains("channel") && body.contains("level"))
			{
				auto channel_name = body["channel"].get<std::string>();
				auto level_name = body["level"].get<std::string>();

				auto channel_cast = magic_enum::enum_cast<Channel>(channel_name);
				auto severity_cast = magic_enum::enum_cast<Severity>(level_name);

				if (!channel_cast.has_value() || !severity_cast.has_value())
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_channel_or_severity", "Invalid channel or severity level");
				}

				SeverityFiltering::SetChannelFilterLevel(channel_cast.value(), severity_cast.value());

				LogInfo(Channel::Web, std::format("Log level for channel {} set to {} via web UI", channel_name, level_name));
			}
			else
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "logging_missing_fields", "Request must contain 'global' or both 'channel' and 'level'");
			}

			return MakeJsonResponse(req, HTTP::Status::ok, R"({"status":"ok"})");
		}
		catch (const nlohmann::json::exception&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}
	}

}
// namespace AqualinkAutomate::HTTP
