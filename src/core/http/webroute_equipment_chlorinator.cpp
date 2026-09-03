#include <cmath>
#include <format>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

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

		constexpr HTTP::Status StatusFor(CommandResult result)
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
			// A capable controller exists but is still applying an earlier chlorinator command
			// (e.g. a duplicate/overlapping "Set" click landed mid-actuation): transient, so the
			// caller should retry shortly rather than read this as the equipment being down.
			case Busy:                return conflict;
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
		auto overall_failure = CommandResult::Success;
		const auto note_failure = [&overall, &overall_failure](CommandResult r)
		{
			if (r != CommandResult::Success && overall == HTTP::Status::ok)
			{
				overall = StatusFor(r);
				overall_failure = r;
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

				// The panel keeps INDEPENDENT pool and spa setpoints, so the caller says which one.
				// `body` is optional and defaults to the pool, which is what the single-value form
				// has always driven -- so existing callers keep their behaviour.
				auto body = Kernel::BodyOfWaterIds::Pool;
				if (payload.contains("body"))
				{
					const auto& body_field = payload["body"];
					if (!body_field.is_string())
					{
						return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "body must be a string");
					}

					const auto parsed = magic_enum::enum_cast<Kernel::BodyOfWaterIds>(body_field.get<std::string>(), magic_enum::case_insensitive);
					if (!parsed.has_value() || ((parsed.value() != Kernel::BodyOfWaterIds::Pool) && (parsed.value() != Kernel::BodyOfWaterIds::Spa)))
					{
						return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "body must be \"pool\" or \"spa\"");
					}

					body = parsed.value();
				}

				const auto value = static_cast<std::uint8_t>(std::round(pct));
				const auto r = m_CommandDispatcher->SetChlorinatorPercentage(value, body);
				note_failure(r);
				result["percentage"] = {
					{ "status", r == CommandResult::Success ? "success" : "error" },
					{ "value", value },
					{ "body", std::string{ magic_enum::enum_name(body) } }
				};
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

		// Surface a reason a duplicate/overlapping request can act on: a Busy chlorinator
		// command is expected to succeed if retried once the in-flight one finishes, which
		// is worth telling the caller explicitly rather than leaving the bare per-field
		// "error" status to speak for itself.
		if (CommandResult::Busy == overall_failure)
		{
			result["error"] = "A previous chlorinator command is still being applied; try again shortly";
			result["code"] = "chlorinator_busy";
		}

		return MakeJsonResponse(req, overall, result.dump());
	}

}
// namespace AqualinkAutomate::HTTP
