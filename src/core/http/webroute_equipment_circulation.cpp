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
#include "http/webroute_equipment_circulation.h"
#include "kernel/circulation.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		using CommandResult = Interfaces::ICommandDispatcher::CommandResult;

		std::optional<Kernel::CirculationModes> ParseMode(std::string_view mode)
		{
			using enum Kernel::CirculationModes;
			if (mode == "pool")      return Pool;
			if (mode == "spa")       return Spa;
			if (mode == "spillover") return Spillover;
			return std::nullopt;
		}
	}
	// unnamed namespace

	WebRoute_Equipment_Circulation::WebRoute_Equipment_Circulation(Kernel::HubLocator& hub_locator)
		: m_CommandDispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>())
	{
	}

	HTTP::Response WebRoute_Equipment_Circulation::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_Circulation::OnRequest", std::source_location::current());

		return HandleJsonCommandRoute(req, m_CommandDispatcher, [&req, this](const nlohmann::json& payload)
		{
			try
			{
				if (!payload.contains("mode") || !payload["mode"].is_string())
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "missing string field 'mode' (pool|spa|spillover)");
				}

				const auto mode_str = payload["mode"].get<std::string>();
				const auto mode = ParseMode(mode_str);
				if (!mode.has_value())
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "'mode' must be one of pool, spa, spillover");
				}

				const auto r = m_CommandDispatcher->SetCirculationMode(mode.value());

				nlohmann::json result = {
					{ "mode", mode_str },
					{ "status", r == CommandResult::Success ? "success" : "error" }
				};

				return MakeJsonResponse(req, StatusForCommandResult(r), result.dump());
			}
			catch (const nlohmann::json::exception& ex)
			{
				LogWarning(Channel::Web, std::format("Circulation POST: JSON access error: {}", ex.what()));
				return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "invalid circulation payload");
			}
		});
	}

}
// namespace AqualinkAutomate::HTTP
