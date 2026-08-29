#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <boost/url/parse.hpp>
#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_captures.h"
#include "http/server/make_response.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// A capture is served by reading it into the response body, because a route
		// hands back a string-bodied Response (Interfaces::IWebRouteBase) rather
		// than a streaming file body.  Cap what that is allowed to cost: at the
		// bus's ~5 KB/s of capture text this is a multi-hour recording, and anything
		// larger is better fetched straight from the capture directory (which the
		// packaged deployments deliberately place somewhere user-browsable) than
		// buffered whole on a Raspberry Pi.
		constexpr std::uintmax_t MAX_DOWNLOAD_BYTES{ 64ULL * 1024ULL * 1024ULL };

		constexpr std::string_view CAPTURE_CONTENT_TYPE{ "text/plain; charset=utf-8" };

		// The capture filename is the LAST path segment of
		// "/api/diagnostics/recording/captures/{filename}".  Re-parse the target as
		// an origin-form URL (the router matched on the same segments) and take the
		// final non-empty, percent-decoded segment.
		std::optional<std::string> CaptureNameFromTarget(const HTTP::Request& req)
		{
			const auto url_result = boost::urls::parse_origin_form(req.target());

			if (!url_result.has_value())
			{
				return std::nullopt;
			}

			std::optional<std::string> last;
			for (const auto& segment : url_result->segments())
			{
				if (!segment.empty())
				{
					last = std::string{ segment };
				}
			}

			return last;
		}

		// Percent-encode for the RFC 5987 `filename*` form, so a non-ASCII capture
		// name survives intact.  Everything outside the unreserved set is escaped.
		std::string PercentEncode(std::string_view value)
		{
			constexpr std::string_view UNRESERVED{ "-._~" };

			std::string encoded;
			encoded.reserve(value.size());

			for (const unsigned char c : value)
			{
				const bool is_alnum = ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'));

				if (is_alnum || (UNRESERVED.find(static_cast<char>(c)) != std::string_view::npos))
				{
					encoded.push_back(static_cast<char>(c));
				}
				else
				{
					encoded += std::format("%{:02X}", c);
				}
			}

			return encoded;
		}
	}

	//=========================================================================
	// GET /api/diagnostics/recording/captures  ->  { "captures": [...] }
	//=========================================================================

	WebRoute_Diagnostics_Captures::WebRoute_Diagnostics_Captures(CaptureDirectory captures)
		: m_Captures(std::move(captures))
	{
	}

	HTTP::Response WebRoute_Diagnostics_Captures::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Captures::OnRequest", std::source_location::current());

		if (Verbs::get != req.method())
		{
			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET.", {{"allowed", "GET"}});
		}

		nlohmann::json captures = nlohmann::json::array();

		for (const auto& entry : m_Captures.List())
		{
			nlohmann::json item;
			// Basename only: the server-side capture directory is never disclosed
			// (the API is unauthenticated unless --auth-mode is enabled).
			item["name"] = entry.name;
			item["bytes"] = entry.bytes;
			item["modified"] = entry.modified_unix;
			captures.push_back(std::move(item));
		}

		nlohmann::json result;
		result["captures"] = std::move(captures);

		return MakeJsonResponse(req, HTTP::Status::ok, result.dump());
	}

	//=========================================================================
	// GET /api/diagnostics/recording/captures/{filename}  ->  the capture file
	//=========================================================================

	WebRoute_Diagnostics_Capture::WebRoute_Diagnostics_Capture(CaptureDirectory captures)
		: m_Captures(std::move(captures))
	{
	}

	HTTP::Response WebRoute_Diagnostics_Capture::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Capture::OnRequest", std::source_location::current());

		if (Verbs::get != req.method())
		{
			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET.", {{"allowed", "GET"}});
		}

		const auto name = CaptureNameFromTarget(req);
		if (!name.has_value() || name->empty())
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "capture_filename_invalid", "'filename' must be a bare *.cap name with no path separators or '..'");
		}

		// SECURITY: the path parameter is client-controlled.  It goes through the
		// SAME jail as the recording route's filename, so this route cannot be
		// turned into an arbitrary-file-read of the host.
		const auto safe_path = m_Captures.ResolveForRead(*name);
		if (!safe_path)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "capture_filename_invalid", "'filename' must be a bare *.cap name with no path separators or '..'");
		}

		std::error_code ec;
		const std::filesystem::path capture{ *safe_path };

		if (!std::filesystem::is_regular_file(capture, ec) || ec)
		{
			LogDebug(Channel::Web, std::format("Capture download requested for a file that does not exist: '{}'", *name));
			return MakeErrorResponse(req, HTTP::Status::not_found, "capture_not_found", "No such capture");
		}

		const auto size = std::filesystem::file_size(capture, ec);
		if (ec)
		{
			LogWarning(Channel::Web, std::format("Could not size capture '{}': {}", *name, ec.message()));
			return MakeErrorResponse(req, HTTP::Status::internal_server_error, "capture_read_failed", "Could not read the capture");
		}

		if (size > MAX_DOWNLOAD_BYTES)
		{
			LogWarning(Channel::Web, std::format("Refusing to serve capture '{}': {} bytes exceeds the {} byte download limit", *name, size, MAX_DOWNLOAD_BYTES));
			return MakeErrorResponse(req, HTTP::Status::payload_too_large, "capture_too_large", "The capture is too large to download; copy it from the capture directory instead", {{"limit_bytes", MAX_DOWNLOAD_BYTES}});
		}

		std::ifstream file{ capture, std::ios::in | std::ios::binary };
		if (!file.is_open())
		{
			LogWarning(Channel::Web, std::format("Could not open capture '{}' for download", *name));
			return MakeErrorResponse(req, HTTP::Status::internal_server_error, "capture_read_failed", "Could not read the capture");
		}

		std::string body;
		body.resize(static_cast<std::size_t>(size));
		file.read(body.data(), static_cast<std::streamsize>(size));

		// A short read is NORMAL here (a recording in progress can be truncated or
		// still growing between sizing and reading), so only a genuine I/O failure
		// is an error; otherwise serve exactly what was actually read.
		if (file.bad())
		{
			LogWarning(Channel::Web, std::format("I/O error reading capture '{}'", *name));
			return MakeErrorResponse(req, HTTP::Status::internal_server_error, "capture_read_failed", "Could not read the capture");
		}

		body.resize(static_cast<std::size_t>(file.gcount()));

		LogDebug(Channel::Web, std::format("Serving capture '{}' ({} bytes)", *name, body.size()));

		auto resp = MakeResponse(req, HTTP::Status::ok, CAPTURE_CONTENT_TYPE, std::move(body));

		// Offer it as a download rather than rendering it in the browser.  The
		// jail already rejects quotes and control characters, so the name is safe
		// in a quoted-string; the RFC 5987 form additionally carries a non-ASCII
		// name through intact.
		resp.set(boost::beast::http::field::content_disposition,
			std::format("attachment; filename=\"{}\"; filename*=UTF-8''{}", *name, PercentEncode(*name)));

		// The body is server-generated bus traffic, but it is client-named and
		// served verbatim: never let a browser sniff it into something executable.
		resp.set("X-Content-Type-Options", "nosniff");

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
