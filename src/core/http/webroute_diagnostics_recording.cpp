#include <filesystem>
#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <system_error>

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
			// is jailed into a fixed capture directory, and echoing the resolved
			// path back to an unauthenticated client would leak the install
			// location.
			result["file"] = std::filesystem::path(status.file).filename().string();
			result["bytes"] = status.bytes_written;
			return result;
		}


		// Fixed, on-disk directory that on-demand captures are confined to.  This
		// is the SOLE location the unauthenticated diagnostics route is permitted
		// to write to, mirroring the gitignored `captures/` scratch convention.
		constexpr std::string_view CAPTURE_SUBDIRECTORY{ "captures" };
		// Required extension for capture files (rejects e.g. ".cap" -> ".conf").
		constexpr std::string_view CAPTURE_EXTENSION{ ".cap" };

		// Validate and "jail" an untrusted, client-supplied recording filename to a
		// fixed capture directory.
		//
		// The value is treated as a BARE BASENAME: any path separator, drive
		// letter, parent-directory token ("..") or leading separator is rejected so
		// the request cannot escape the capture directory (path traversal /
		// arbitrary-file-write/truncate).  The basename is then joined onto the
		// fixed capture directory and the weakly-canonical result is verified to
		// remain inside it (mirrors HTTP::StaticFileHandler::match's path jail).
		//
		// @returns the safe absolute path to open on success; std::nullopt (with a
		//          one-line reason logged) when the filename is rejected.
		std::optional<std::string> JailRecordingFilename(const std::string& filename)
		{
			// Reject any character that introduces a path on EITHER platform up
			// front, so a Windows-style traversal cannot slip through on a POSIX
			// host (where '\\' and ':' are ordinary filename characters and would
			// not be parsed as separators by std::filesystem).
			if (filename.find('/') != std::string::npos ||
				filename.find('\\') != std::string::npos ||
				filename.find(':') != std::string::npos)
			{
				LogWarning(Channel::Web, std::format("Rejected recording filename (contains a path separator or drive specifier): '{}'", filename));
				return std::nullopt;
			}

			// Reject anything that is not a self-contained basename.  std::filesystem
			// is locale/separator-aware, so checking the parsed components catches
			// both POSIX ('/') and Windows ('\\', drive letters) traversal forms.
			const std::filesystem::path candidate{ filename };

			if (candidate.has_parent_path() ||      // contains a separator (a/b, /a, ../a, C:\a)
				candidate.has_root_name() ||         // drive letter / UNC root (C:, \\host)
				candidate.has_root_directory() ||    // leading separator
				candidate.filename() != candidate)   // any extra component / trailing separator
			{
				LogWarning(Channel::Web, std::format("Rejected recording filename (not a bare basename): '{}'", filename));
				return std::nullopt;
			}

			// Defence in depth: explicitly reject the parent-directory token even
			// though has_parent_path() above already catches separated forms.
			const std::string basename = candidate.filename().string();
			if (basename == "." || basename == ".." || basename.find("..") != std::string::npos)
			{
				LogWarning(Channel::Web, std::format("Rejected recording filename (parent-directory token): '{}'", filename));
				return std::nullopt;
			}

			// Enforce the capture extension so the sink cannot be steered at, say,
			// a config or executable name within the capture directory.
			if (!candidate.has_extension() || candidate.extension().string() != CAPTURE_EXTENSION)
			{
				LogWarning(Channel::Web, std::format("Rejected recording filename (must end in '{}'): '{}'", CAPTURE_EXTENSION, filename));
				return std::nullopt;
			}

			// Join onto the fixed capture directory and confirm the canonical result
			// stays inside it (belt-and-braces against any residual traversal).
			std::error_code ec;
			const auto capture_dir = std::filesystem::absolute(std::filesystem::path{ CAPTURE_SUBDIRECTORY }, ec);
			if (ec)
			{
				LogWarning(Channel::Web, std::format("Could not resolve capture directory: {}", ec.message()));
				return std::nullopt;
			}

			// Best-effort create the capture directory; opening will surface any
			// real failure as a 409 from the controller.
			std::filesystem::create_directories(capture_dir, ec);

			const auto target = capture_dir / basename;

			const auto canonical_target = std::filesystem::weakly_canonical(target, ec);
			if (ec)
			{
				LogWarning(Channel::Web, std::format("Capture path canonicalisation failed for '{}': {}", filename, ec.message()));
				return std::nullopt;
			}
			const auto canonical_root = std::filesystem::weakly_canonical(capture_dir, ec);
			if (ec)
			{
				LogWarning(Channel::Web, std::format("Capture directory canonicalisation failed: {}", ec.message()));
				return std::nullopt;
			}

			// The resolved path must be lexically a descendant of the capture root.
			if (const auto relative = canonical_target.lexically_relative(canonical_root);
				relative.empty() || *relative.begin() == "..")
			{
				LogWarning(Channel::Web, std::format("Rejected recording filename (escapes capture directory): '{}'", filename));
				return std::nullopt;
			}

			return canonical_target.string();
		}
	}

	// TryFind (not Find): the recording controller is only present in the
	// production serial chain. In dev-mode/replay there is none, and the route
	// should still construct and report recording=false rather than throw.
	WebRoute_Diagnostics_Recording::WebRoute_Diagnostics_Recording(Kernel::HubLocator& hub_locator)
		: m_RecordingController(hub_locator.TryFind<Interfaces::IRecordingController>())
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

	HTTP::Response WebRoute_Diagnostics_Recording::HandleGet(const HTTP::Request& req)
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

	HTTP::Response WebRoute_Diagnostics_Recording::HandlePost(const HTTP::Request& req)
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

				// SECURITY: the filename is attacker-controlled (unauthenticated POST).
				// Jail it to a fixed capture directory as a bare basename before it
				// reaches the file-opening code, otherwise it is an arbitrary
				// file-write/truncate sink via path traversal (e.g. "../../etc/x").
				const auto safe_path = JailRecordingFilename(filename);
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
