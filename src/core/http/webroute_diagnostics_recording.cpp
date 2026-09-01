#include <filesystem>
#include <format>
#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_recording.h"
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
		//   { "recording": bool, "file": string, "bytes": number }
		nlohmann::json StatusToJson(const Interfaces::IRecordingController::Status& status)
		{
			nlohmann::json result;
			result["recording"] = status.recording;
			// Report only the basename, never the absolute server path: the value
			// is jailed into the configured capture directory, and echoing the
			// resolved path back to a client (the API is unauthenticated unless
			// --auth-mode is enabled) would leak the install location.
			result["file"] = std::filesystem::path(status.file).filename().string();
			result["bytes"] = status.bytes_written;
			return result;
		}
	}

	// TryFind (not Find): the recording controller is only present in the
	// production serial chain. In dev-mode/replay there is none, and the route
	// should still construct and report recording=false rather than throw.
	WebRoute_Diagnostics_Recording::WebRoute_Diagnostics_Recording(Kernel::HubLocator& hub_locator, CaptureDirectory captures)
		: m_RecordingController(hub_locator.TryFind<Interfaces::IRecordingController>())
		, m_Captures(std::move(captures))
	{
	}

	HTTP::Response WebRoute_Diagnostics_Recording::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Recording::OnRequest", std::source_location::current());

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

	HTTP::Response WebRoute_Diagnostics_Recording::HandleGet(const HTTP::Request& req) const
	{
		Interfaces::IRecordingController::Status status;
		if (m_RecordingController)
		{
			status = m_RecordingController->RecordingStatus();
		}
		// When no controller is registered the default-constructed status reports
		// recording=false / empty file / 0 bytes, which is the correct picture.

		return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(status).dump());
	}

	HTTP::Response WebRoute_Diagnostics_Recording::HandlePost(const HTTP::Request& req) const
	{
		if (!m_RecordingController)
		{
			LogWarning(Channel::Web, "Recording toggle requested but no recording controller is available (dev-mode/replay?)");
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "recording_unavailable", "Recording is not available in this mode");
		}

		try
		{
			auto body = nlohmann::json::parse(req.body());

			if (!body.contains("action") || !body["action"].is_string())
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "recording_action_required", "Request must contain a string 'action' of 'start' or 'stop'");
			}

			if (const auto action = body["action"].get<std::string>(); "start" == action)
			{
				if (!body.contains("filename") || !body["filename"].is_string())
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "recording_filename_required", "'start' action requires a string 'filename'");
				}

				const auto filename = body["filename"].get<std::string>();
				if (filename.empty())
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "recording_filename_empty", "'filename' must not be empty");
				}

				// SECURITY: the filename is client-controlled (and the API is
				// unauthenticated unless --auth-mode is enabled).  Jail it to the
				// configured capture directory as a bare basename before it reaches
				// the file-opening code, otherwise it is an arbitrary
				// file-write/truncate sink via path traversal (e.g. "../../etc/x").
				const auto safe_path = m_Captures.ResolveForWrite(filename);
				if (!safe_path)
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "recording_filename_invalid", "'filename' must be a bare *.cap name with no path separators or '..'");
				}

				if (!m_RecordingController->StartRecording(*safe_path))
				{
					// Already recording, or the file could not be opened.
					LogWarning(Channel::Web, std::format("Failed to start serial recording to '{}' via web UI", *safe_path));
					return MakeErrorResponse(req, HTTP::Status::conflict, "recording_start_failed", "Could not start recording: already recording or file could not be opened");
				}

				LogInfo(Channel::Web, std::format("Serial recording started to '{}' via web UI", *safe_path));
			}
			else if ("stop" == action)
			{
				if (!m_RecordingController->StopRecording())
				{
					LogDebug(Channel::Web, "Stop recording requested via web UI but nothing was recording");
					return MakeErrorResponse(req, HTTP::Status::conflict, "recording_not_recording", "Not currently recording");
				}

				LogInfo(Channel::Web, "Serial recording stopped via web UI");
			}
			else
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "recording_invalid_action", "'action' must be 'start' or 'stop'");
			}

			// Success: return the up-to-date status.
			return MakeJsonResponse(req, HTTP::Status::ok, StatusToJson(m_RecordingController->RecordingStatus()).dump());
		}
		catch (const nlohmann::json::exception&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}
	}

}
// namespace AqualinkAutomate::HTTP
