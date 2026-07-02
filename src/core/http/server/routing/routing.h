#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/subject.h"
#include "http/server/server_types.h"
#include "http/server/static_file_handler.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"

namespace AqualinkAutomate::HTTP::Routing
{

	/// Optional, opt-in security policy for the HTTP/WebSocket control plane.
	///
	/// Every knob defaults to "disabled" so a default-constructed SecurityConfig
	/// reproduces the historical behaviour exactly: no authentication, no Origin
	/// check, no CSRF requirement.  The policy is consulted by HTTP_OnRequest (for
	/// HTTP requests) and AuthorizeWebSocketUpgrade (for the WebSocket handshake).
	struct SecurityConfig
	{
		/// Shared bearer token. When unset (the default) no authentication is
		/// performed. When set, requests must carry "Authorization: Bearer <token>"
		/// and the token is compared in constant time.
		std::optional<std::string> AuthToken{ std::nullopt };

		/// Allow-list of acceptable Origin header values for cross-site protection.
		/// When empty (the default) the Origin header is not checked. When non-empty,
		/// a request/upgrade carrying an Origin not in the list is rejected.
		std::vector<std::string> AllowedOrigins{};

		/// When true, state-changing requests (POST/PUT/PATCH/DELETE) must carry a
		/// non-empty custom header (X-Requested-With) as a CSRF mitigation. Defaults
		/// to false so existing clients are unaffected.
		bool RequireCsrfHeader{ false };

		/// CIDR blocks (e.g. "10.0.0.0/8", "::1/128") of TRUSTED reverse proxies.
		/// Only when the connecting peer is inside one of these does the router
		/// honour X-Forwarded-For to derive the real client address (for the
		/// failed-auth rate limiter and audit identity).  Empty (the default):
		/// XFF is ignored entirely — an untrusted client cannot spoof its way
		/// out of (or someone else into) a rate-limit bucket.
		std::vector<std::string> TrustedProxyCidrs{};

		/// When true the identity system (--auth-mode enabled) governs requests:
		/// every request is resolved to an Auth::Subject (see SetSubjectResolver)
		/// and routes declaring an AccessRequirement are gated by the
		/// Auth::PolicyEngine.  The legacy shared-token check (AuthToken above) is
		/// then SUPERSEDED — bearer credentials are interpreted by the resolver
		/// instead (the legacy token folds in as a bootstrap API key in Slice 2).
		/// Defaults to false: historical behaviour, decisions all Permit (posture).
		bool AuthModeEnabled{ false };

		/// True when any security knob is engaged. Used to early-out of the checks on
		/// the request hot path when the feature is entirely disabled (the default).
		[[nodiscard]] bool IsEnabled() const noexcept
		{
			return AuthToken.has_value() || !AllowedOrigins.empty() || RequireCsrfHeader || AuthModeEnabled;
		}
	};

	/// Resolve the credentials on a request (session JWT / API key / forwarded
	/// header / nothing) into the Auth::Subject the PolicyEngine consumes.  The
	/// providers are wired per-slice; routing only depends on this signature.
	using SubjectResolver = std::function<Auth::Subject(const HTTP::Request& req, bool is_websocket_upgrade)>;

	/// Install (or clear, by passing nullptr) the active subject resolver.  With
	/// none installed every request resolves to Auth::Subject::Anonymous().
	void SetSubjectResolver(SubjectResolver resolver);

	/// Derive the effective client address for rate-limiting/audit: the peer
	/// address, unless the peer is a trusted proxy (SecurityConfig::
	/// TrustedProxyCidrs) offering X-Forwarded-For — then the FIRST forwarded
	/// hop.  Exposed for tests; HTTP_OnRequest applies it automatically.
	std::string EffectiveClientIp(const HTTP::Request& req, std::string_view peer_ip);

	/// The Subject resolved for the request currently being dispatched.  Valid
	/// only during HTTP_OnRequest/AuthorizeWebSocketUpgrade (single-threaded
	/// cooperative model); route handlers use it for per-resource authorization
	/// refinement and subject-aware responses (/api/auth/me, preferences).
	const Auth::Subject& CurrentSubject();

	void Clear();

	void Add(std::unique_ptr<Interfaces::IWebRouteBase>&& handler);
	void Add(std::unique_ptr<Interfaces::IWebSocketBase>&& handler);

	void StaticHandler(StaticFileHandler&& sf);

	/// Install (or replace) the active security policy. Reset to the disabled
	/// default by Clear(). The HttpServer wires this from the Web settings.
	void SetSecurityConfig(SecurityConfig config);
	const SecurityConfig& GetSecurityConfig();

	/// peer_ip (the connecting client's address, empty when unknown) feeds the
	/// per-source failed-auth rate limiter; pass it from the HTTP session.
	HTTP::Message HTTP_OnRequest(const HTTP::Request& req, std::string_view peer_ip = {});
	Interfaces::IWebSocketBase* WS_OnAccept(const std::string_view target);

	/// Evaluate the security policy against a WebSocket upgrade request.
	/// Returns std::nullopt when the upgrade is permitted; otherwise returns the
	/// HTTP error response (401/403/429) that should be written instead of accepting.
	std::optional<HTTP::Response> AuthorizeWebSocketUpgrade(const HTTP::Request& req, std::string_view peer_ip = {});

}
// namespace AqualinkAutomate::HTTP::Routing
