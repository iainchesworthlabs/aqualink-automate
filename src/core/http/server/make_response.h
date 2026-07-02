#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "http/server/server_types.h"

namespace AqualinkAutomate::HTTP
{

	/// @brief Build a fully-formed HTTP response with the standard server header set.
	///
	/// Centralises the response-construction boilerplate (Server header, Content-Type,
	/// keep-alive mirroring and prepare_payload()) that was previously duplicated across
	/// every webroute handler. The body is moved into the response.
	///
	/// @param req           The originating request (used for HTTP version and keep-alive).
	/// @param code          The HTTP status code to return.
	/// @param content_type  The Content-Type header value (see ContentTypes).
	/// @param body          The response body (moved in).
	Response MakeResponse(const Request& req, Status code, std::string_view content_type, std::string body);

	/// @brief Convenience wrapper for an application/json response.
	///
	/// Equivalent to MakeResponse(req, code, ContentTypes::APPLICATION_JSON, std::move(body)).
	Response MakeJsonResponse(const Request& req, Status code, std::string body);

	/// @brief Structured JSON error response.
	///
	/// Emits {"error": <message>, "code": <error_code>[, "params": <params>]}.
	/// `error` remains the human-readable ENGLISH message — the stable contract
	/// for direct API consumers and log greps. `code` is a stable snake_case
	/// identifier the web UI maps to a translated message (catalog key
	/// `error.<code>`, docs/i18n.md); `params` carries any values that
	/// translation interpolates and is omitted when empty. Adding a code here
	/// means adding the matching key to every locale catalog.
	Response MakeErrorResponse(const Request& req, Status code, std::string_view error_code, std::string_view message, const nlohmann::json& params = nlohmann::json::object());

}
// namespace AqualinkAutomate::HTTP
