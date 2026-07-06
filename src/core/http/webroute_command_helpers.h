#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "interfaces/icommanddispatcher.h"

// Shared helpers for the equipment command web routes (heater, circulation, ...). These collapse
// the boilerplate every command route repeats (result->status mapping, the no-dispatcher guard,
// and JSON-object body parsing) into one place so each route only carries its own field handling.
namespace AqualinkAutomate::HTTP
{
	// Map a command-dispatch result to the HTTP status a route should return.
	constexpr HTTP::Status StatusForCommandResult(Interfaces::ICommandDispatcher::CommandResult result)
	{
		using enum Interfaces::ICommandDispatcher::CommandResult;
		using enum HTTP::Status;
		switch (result)
		{
		case Success:              return ok;
		case InvalidValue:         return bad_request;
		case DeviceNotFound:
		case NoSerialAdapter:      return service_unavailable;
		case UnknownEquipmentType: return unprocessable_entity;
		default:                   return internal_server_error;
		}
	}

	// Returns a 503 response when no command dispatcher is wired, else std::nullopt.
	inline std::optional<HTTP::Response> RequireCommandDispatcher(const HTTP::Request& req,
		const std::shared_ptr<Interfaces::ICommandDispatcher>& dispatcher)
	{
		if (!dispatcher)
		{
			return MakeResponse(req, HTTP::Status::service_unavailable, ContentTypes::TEXT_PLAIN, "Command dispatcher not available");
		}
		return std::nullopt;
	}

	// Parse the request body as a JSON object into `out`; returns a 400 response on failure.
	inline std::optional<HTTP::Response> ParseJsonObjectBody(const HTTP::Request& req, nlohmann::json& out)
	{
		out = nlohmann::json::parse(req.body(), nullptr, /*allow_exceptions=*/false);
		if (out.is_discarded() || !out.is_object())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "request body must be a JSON object");
		}
		return std::nullopt;
	}

	// Drive a POST-only JSON-command route end-to-end: 405 for non-POST, 503 when no command
	// dispatcher is wired, 400 for a non-object body; otherwise invoke `handle(payload)` with the
	// parsed JSON object and return its response. Each route supplies only its own field handling.
	template<typename Handler>
	HTTP::Response HandleJsonCommandRoute(const HTTP::Request& req,
		const std::shared_ptr<Interfaces::ICommandDispatcher>& dispatcher, Handler&& handle)
	{
		if (req.method() != HTTP::Verbs::post)
		{
			return HTTP::Responses::Response_405(req);
		}

		if (auto err = RequireCommandDispatcher(req, dispatcher); err.has_value())
		{
			return std::move(*err);
		}

		nlohmann::json payload;
		if (auto err = ParseJsonObjectBody(req, payload); err.has_value())
		{
			return std::move(*err);
		}

		return handle(payload);
	}
}
// namespace AqualinkAutomate::HTTP
