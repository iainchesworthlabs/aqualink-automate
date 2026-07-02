#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

#include <nlohmann/json.hpp>

#include "http/webroute_equipment_setpoints.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "utility/json_serialization_helpers.h"
#include "http/server/responses/response_405.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace
{
	// Documented Celsius bounds for a pool/spa water-temperature setpoint received over the API.
	//
	// The /api/equipment/setpoints payload carries values in Celsius (see the "celsius" field in
	// the response). These bounds reject non-finite and absurd attacker-controlled values BEFORE
	// the Celsius->wire conversion, so the subsequent double->uint8_t cast can never observe an
	// out-of-range double (which is undefined behaviour). Valid Jandy pool/spa setpoints sit well
	// inside this range (~4-40C); the window is kept slightly generous to tolerate rounding.
	//
	// NOTE (cross-unit): WU-HTTP-RESPONSE-BUILDER introduces Utility::CelsiusToWireSetpoint in a
	// new src/core/utility/temperature_conversion.h. That header is owned by another work unit and
	// is not yet present, so the conversion + clamp are implemented defensively inline here and
	// should be routed through the shared helper once it lands.
	constexpr double SETPOINT_CELSIUS_MIN{ -10.0 };
	constexpr double SETPOINT_CELSIUS_MAX{ 50.0 };
}

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_Setpoints::WebRoute_Equipment_Setpoints(Kernel::HubLocator& hub_locator)
	{
		m_DataHub = hub_locator.Find<Kernel::DataHub>();
		m_CommandDispatcher = hub_locator.TryFind<Interfaces::ICommandDispatcher>();
	}

	HTTP::Response WebRoute_Equipment_Setpoints::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_EquipmentSetpoints::OnRequest", std::source_location::current());

		switch (req.method())
		{
		case HTTP::Verbs::get:
			return HandleGet(req);

		case HTTP::Verbs::post:
			return HandlePost(req);

		default:
			return HTTP::Responses::Response_405(req);
		}
	}

	HTTP::Response WebRoute_Equipment_Setpoints::HandleGet(const HTTP::Request& req)
	{
		nlohmann::json body;

		body["pool_setpoint"] = Utility::SerializeTemperature(m_DataHub->PoolTempSetpoint(), m_DataHub->PoolTempSetpointUpdatedAt(), false);
		// Second pool setpoint (POOLSP2 / panel "TEMP2"), present only on single-body systems;
		// null otherwise. Read-only -- the POOLSP2 write path is not yet validated on live hardware.
		body["pool_setpoint_2"] = Utility::SerializeTemperature(m_DataHub->PoolTempSetpoint2());
		// Whether TEMP2 maintenance heating (POOLHT2) is enabled; null when not reported. Read-only.
		body["pool_heater_2_enabled"] = m_DataHub->PoolHeater2Enabled().has_value()
			? nlohmann::json(m_DataHub->PoolHeater2Enabled().value()) : nlohmann::json(nullptr);
		body["spa_setpoint"] = Utility::SerializeTemperature(m_DataHub->SpaTempSetpoint(), m_DataHub->SpaTempSetpointUpdatedAt(), false);

		HTTP::Response resp{ HTTP::Status::ok, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
		resp.keep_alive(req.keep_alive());
		resp.body() = body.dump();
		resp.prepare_payload();

		return resp;
	}

	HTTP::Response WebRoute_Equipment_Setpoints::HandlePost(const HTTP::Request& req)
	{
		if (!m_CommandDispatcher)
		{
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "dispatcher_unavailable", "Command dispatcher not available");
		}

		nlohmann::json payload;
		try
		{
			payload = nlohmann::json::parse(req.body());
		}
		catch (const nlohmann::json::parse_error&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}

		nlohmann::json result;
		bool any_dispatch_failed = false;

		// Returns std::nullopt on success; otherwise the bad-request response to send immediately.
		auto convert_and_dispatch = [&](const std::string& key, auto dispatch_fn) -> std::optional<HTTP::Response>
		{
			if (!payload.contains(key))
			{
				return std::nullopt; // field not present = OK, skip
			}

			const auto& field = payload[key];
			if (!field.is_number())
			{
				LogWarning(Channel::Web, std::format("Setpoints POST: '{}' field is not a number", key));
				return MakeErrorResponse(req, HTTP::Status::bad_request, "setpoint_not_a_number", std::format("Setpoint '{}' must be a number", key), {{"target", key}});
			}

			double celsius = field.get<double>();

			if (!std::isfinite(celsius) || (celsius < SETPOINT_CELSIUS_MIN) || (celsius > SETPOINT_CELSIUS_MAX))
			{
				LogWarning(Channel::Web, std::format("Setpoints POST: '{}' value {} out of range [{}, {}]", key, celsius, SETPOINT_CELSIUS_MIN, SETPOINT_CELSIUS_MAX));
				return MakeErrorResponse(req, HTTP::Status::bad_request, "setpoint_out_of_range", std::format("Setpoint '{}' out of range", key), {{"target", key}, {"min", SETPOINT_CELSIUS_MIN}, {"max", SETPOINT_CELSIUS_MAX}});
			}

			// Convert to the system's configured units, then clamp to the uint8_t domain BEFORE the
			// cast so an out-of-range double can never trigger undefined behaviour. The range check
			// above already guarantees this, but the clamp is kept as defence-in-depth.
			double wire_value = (m_DataHub->SystemTemperatureUnits() == Kernel::TemperatureUnits::Celsius)
				? std::round(celsius)
				: std::round(celsius * 9.0 / 5.0 + 32.0);

			wire_value = std::clamp(wire_value, 0.0, 255.0);
			auto temp_value = static_cast<uint8_t>(wire_value);

			auto cmd_result = dispatch_fn(temp_value);
			bool succeeded = (cmd_result == Interfaces::ICommandDispatcher::CommandResult::Success);
			any_dispatch_failed = any_dispatch_failed || !succeeded;

			result[key] = {
				{"status", succeeded ? "success" : "error"},
				{"celsius", celsius}
			};

			return std::nullopt;
		};

		try
		{
			if (auto err = convert_and_dispatch("pool", [&](uint8_t temp) { return m_CommandDispatcher->SetPoolSetpoint(temp); }); err.has_value())
			{
				return std::move(*err);
			}

			if (auto err = convert_and_dispatch("spa", [&](uint8_t temp) { return m_CommandDispatcher->SetSpaSetpoint(temp); }); err.has_value())
			{
				return std::move(*err);
			}
		}
		catch (const nlohmann::json::exception& ex)
		{
			// Defence-in-depth: any JSON type/access error maps to a client error, never a 500.
			LogWarning(Channel::Web, std::format("Setpoints POST: JSON access error: {}", ex.what()));
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_setpoint_payload", "Invalid setpoint payload");
		}

		// Surface a non-2xx status when any per-key dispatch failed so the UI's resp.ok is
		// authoritative; the per-key JSON body still carries the individual statuses.
		HTTP::Response resp{ any_dispatch_failed ? HTTP::Status::internal_server_error : HTTP::Status::ok, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
		resp.keep_alive(req.keep_alive());
		resp.body() = result.dump();
		resp.prepare_payload();

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
