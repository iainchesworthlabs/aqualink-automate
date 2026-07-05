#include <cmath>
#include <format>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_equipment_chlorinator.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		using CommandResult = Interfaces::ICommandDispatcher::CommandResult;

		HTTP::Status StatusFor(CommandResult result)
		{
			using enum CommandResult;
			using enum HTTP::Status;
			switch (result)
			{
			case Success:             return ok;
			case InvalidValue:        return bad_request;
			case DeviceNotFound:
			case NoSerialAdapter:     return service_unavailable;
			case UnknownEquipmentType:return unprocessable_entity;
			default:                  return internal_server_error;
			}
		}
	}
	// unnamed namespace

	WebRoute_Equipment_Chlorinator::WebRoute_Equipment_Chlorinator(Kernel::HubLocator& hub_locator) :
		m_CommandDispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>())
	{
	}

	HTTP::Response WebRoute_Equipment_Chlorinator::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_EquipmentChlorinator::OnRequest", std::source_location::current());

		if (req.method() == HTTP::Verbs::post)
		{
			return HandlePost(req);
		}
		return HTTP::Responses::Response_405(req);
	}

	HTTP::Response WebRoute_Equipment_Chlorinator::HandlePost(const HTTP::Request& req) const
	{
		if (!m_CommandDispatcher)
		{
			return MakeResponse(req, HTTP::Status::service_unavailable, ContentTypes::TEXT_PLAIN, "Command dispatcher not available");
		}

		auto payload = nlohmann::json::parse(req.body(), nullptr, /*allow_exceptions=*/false);
		if (payload.is_discarded() || !payload.is_object())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "request body must be a JSON object");
		}

		nlohmann::json result;
		HTTP::Status overall = HTTP::Status::ok;
		const auto note_failure = [&overall](CommandResult r)
		{
			if (r != CommandResult::Success && overall == HTTP::Status::ok)
			{
				overall = StatusFor(r);
			}
		};

		try
		{
			if (payload.contains("percentage"))
			{
				const auto& field = payload["percentage"];
				if (!field.is_number())
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "percentage must be a number");
				}
				const double pct = field.get<double>();
				if (!std::isfinite(pct) || pct < 0.0 || pct > 100.0)
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "percentage must be 0..100");
				}
				const auto value = static_cast<std::uint8_t>(std::round(pct));
				const auto r = m_CommandDispatcher->SetChlorinatorPercentage(value);
				note_failure(r);
				result["percentage"] = { { "status", r == CommandResult::Success ? "success" : "error" }, { "value", value } };
			}

			if (payload.contains("boost"))
			{
				const auto& field = payload["boost"];
				if (!field.is_boolean())
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "boost must be a boolean");
				}
				const bool enable = field.get<bool>();
				const auto r = m_CommandDispatcher->SetChlorinatorBoost(enable);
				note_failure(r);
				result["boost"] = { { "status", r == CommandResult::Success ? "success" : "error" }, { "value", enable } };
			}
		}
		catch (const nlohmann::json::exception& ex)
		{
			LogWarning(Channel::Web, std::format("Chlorinator POST: JSON access error: {}", ex.what()));
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "invalid chlorinator payload");
		}

		return MakeJsonResponse(req, overall, result.dump());
	}

}
// namespace AqualinkAutomate::HTTP
