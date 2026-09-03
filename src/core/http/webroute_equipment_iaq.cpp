#include <format>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_command_helpers.h"
#include "http/webroute_equipment_iaq.h"
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
			case Success:              return ok;
			case InvalidValue:         return bad_request;
			case DeviceNotFound:
			case NoSerialAdapter:      return service_unavailable;
			case UnknownEquipmentType: return unprocessable_entity;
			// A capable controller exists but is still applying an earlier command: transient,
			// so the caller should retry shortly rather than read this as the equipment being down.
			case Busy:                 return conflict;
			default:                   return internal_server_error;
			}
		}
	}
	// unnamed namespace

	WebRoute_Equipment_IAQ::WebRoute_Equipment_IAQ(Kernel::HubLocator& hub_locator) :
		m_CommandDispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>())
	{
	}

	HTTP::Response WebRoute_Equipment_IAQ::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_IAQ::OnRequest", std::source_location::current());

		if (req.method() == HTTP::Verbs::post)
		{
			return HandlePost(req);
		}
		return HTTP::Responses::Response_405(req);
	}

	HTTP::Response WebRoute_Equipment_IAQ::HandlePost(const HTTP::Request& req) const
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

		try
		{
			if (payload.contains("select_button"))
			{
				const auto& field = payload["select_button"];
				if (!field.is_number_integer())
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "select_button must be an integer");
				}
				const auto index = field.get<int>();
				if (index < 0 || index > 255)
				{
					return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "select_button must be 0..255");
				}
				const auto r = m_CommandDispatcher->SelectIAQPageButton(static_cast<std::uint8_t>(index));
				if (r != CommandResult::Success)
				{
					overall = StatusFor(r);
				}
				result["select_button"] = { { "status", r == CommandResult::Success ? "success" : "error" }, { "value", index } };
				NoteBusyReason(result, r, "iaq_busy", "A previous command is still being applied; try again shortly");
			}
		}
		catch (const nlohmann::json::exception& ex)
		{
			LogWarning(Channel::Web, std::format("IAQ POST: JSON access error: {}", ex.what()));
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "invalid IAQ payload");
		}

		return MakeJsonResponse(req, overall, result.dump());
	}

}
// namespace AqualinkAutomate::HTTP
