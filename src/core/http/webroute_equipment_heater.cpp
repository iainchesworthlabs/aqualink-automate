#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_command_helpers.h"
#include "http/webroute_equipment_heater.h"
#include "kernel/body_of_water_ids.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		using CommandResult = Interfaces::ICommandDispatcher::CommandResult;

		// Map the "body" field to a heater's body of water. Solar has no body of its own and is
		// modelled as the Shared heater.
		std::optional<Kernel::BodyOfWaterIds> ParseHeaterBody(std::string_view body)
		{
			using enum Kernel::BodyOfWaterIds;
			if (body == "pool")  return Pool;
			if (body == "spa")   return Spa;
			if (body == "solar") return Shared;
			return std::nullopt;
		}
	}
	// unnamed namespace

	WebRoute_Equipment_Heater::WebRoute_Equipment_Heater(Kernel::HubLocator& hub_locator)
		: m_CommandDispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>())
	{
	}

	HTTP::Response WebRoute_Equipment_Heater::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_Heater::OnRequest", std::source_location::current());

		return HandleJsonCommandRoute(req, m_CommandDispatcher, [&req, this](const nlohmann::json& payload)
		{
			return HandleHeaterCommand(req, payload);
		});
	}

	HTTP::Response WebRoute_Equipment_Heater::HandleHeaterCommand(const HTTP::Request& req, const nlohmann::json& payload)
	{
		try
		{
			if (!payload.contains("body") || !payload["body"].is_string())
			{
				return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "missing string field 'body' (pool|spa|solar)");
			}
			if (!payload.contains("enable") || !payload["enable"].is_boolean())
			{
				return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "missing boolean field 'enable'");
			}

			const auto body_str = payload["body"].get<std::string>();
			const auto enable = payload["enable"].get<bool>();

			const auto heater_body = ParseHeaterBody(body_str);
			if (!heater_body.has_value())
			{
				return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "'body' must be one of pool, spa, solar");
			}

			const auto r = m_CommandDispatcher->SetHeaterMode(heater_body.value(), enable);

			nlohmann::json result = {
				{ "body", body_str },
				{ "enable", enable },
				{ "status", r == CommandResult::Success ? "success" : "error" }
			};

			return MakeJsonResponse(req, StatusForCommandResult(r), result.dump());
		}
		catch (const nlohmann::json::exception& ex)
		{
			LogWarning(Channel::Web, std::format("Heater POST: JSON access error: {}", ex.what()));
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "invalid heater payload");
		}
	}

}
// namespace AqualinkAutomate::HTTP
