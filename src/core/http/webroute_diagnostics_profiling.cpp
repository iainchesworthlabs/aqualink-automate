#include <format>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_profiling.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// Build the status envelope shared by GET and POST responses:
		//   { "enabled": bool, "running": bool, "backend": string, "available": [string] }
		nlohmann::json StatusToJson(const Interfaces::IProfilingController::Status& status)
		{
			nlohmann::json result;
			result["enabled"] = status.enabled;
			result["running"] = status.running;
			result["backend"] = status.active_backend;
			result["available"] = status.available_backends;
			return result;
		}

	}

	// TryFind (not Find): keep the route resilient even if the controller was
	// not registered — it then reports enabled=false and rejects toggles.
	WebRoute_Diagnostics_Profiling::WebRoute_Diagnostics_Profiling(Kernel::HubLocator& hub_locator) :
		m_ProfilingController(hub_locator.TryFind<Interfaces::IProfilingController>())
	{
	}

	HTTP::Response WebRoute_Diagnostics_Profiling::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Profiling::OnRequest", std::source_location::current());

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

	HTTP::Response WebRoute_Diagnostics_Profiling::HandleGet(const HTTP::Request& req) const
	{
		Interfaces::IProfilingController::Status status;
		if (m_ProfilingController)
		{
			status = m_ProfilingController->ProfilingStatus();
		}
		// With no controller the default-constructed status reports
		// enabled=false / running=false / no backends, which is the correct picture.

		return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(status).dump());
	}

	HTTP::Response WebRoute_Diagnostics_Profiling::HandlePost(const HTTP::Request& req) const
	{
		if (!m_ProfilingController)
		{
			LogWarning(Channel::Web, "Profiling control requested but no profiling controller is available");
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "profiling_unavailable", "Profiling control is not available");
		}

		try
		{
			auto body = nlohmann::json::parse(req.body());

			if (!body.contains("action") || !body["action"].is_string())
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "profiling_action_required", "Request must contain a string 'action' of 'start', 'stop' or 'select'");
			}

			if (const auto action = body["action"].get<std::string>(); "start" == action)
			{
				if (!m_ProfilingController->Start())
				{
					return MakeErrorResponse(req, HTTP::Status::conflict, "profiling_no_backend", "No profiling backend is available in this build (ENABLE_PROFILING=OFF or backend not selected)");
				}
				LogInfo(Channel::Web, "Profiling capture resumed via web UI");
			}
			else if ("stop" == action)
			{
				if (!m_ProfilingController->Stop())
				{
					return MakeErrorResponse(req, HTTP::Status::conflict, "profiling_no_backend", "No profiling backend is available in this build (ENABLE_PROFILING=OFF or backend not selected)");
				}
				LogInfo(Channel::Web, "Profiling capture paused via web UI");
			}
			else if ("select" == action)
			{
				if (!body.contains("backend") || !body["backend"].is_string())
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "profiling_select_requires_backend", "'select' action requires a string 'backend' (tracy, uprof or vtune)");
				}

				const auto backend = body["backend"].get<std::string>();
				if (!m_ProfilingController->SelectBackend(backend))
				{
					return MakeErrorResponse(req, HTTP::Status::conflict, "profiling_unknown_backend", "Requested backend is unknown or was not compiled into this build");
				}
				LogInfo(Channel::Web, std::format("Profiling backend '{}' selected via web UI", backend));
			}
			else
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "profiling_invalid_action", "'action' must be 'start', 'stop' or 'select'");
			}

			// Success: return the up-to-date status.
			return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(m_ProfilingController->ProfilingStatus()).dump());
		}
		catch (const nlohmann::json::exception&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}
	}

}
// namespace AqualinkAutomate::HTTP
