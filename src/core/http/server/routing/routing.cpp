#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/parse_path.hpp>
#include <boost/url/url.hpp>
#include <magic_enum/magic_enum.hpp>

#include "auth/policy_engine.h"
#include "auth/subject.h"
#include "exceptions/exception_http_duplicateroute.h"
#include "formatters/beast_stringview_formatter.h"
#include "formatters/url_segments_encoded_view_formatter.h"
#include "http/server/server_fields.h"
#include "http/server/static_file_handler.h"
#include "http/server/responses/response_staticfile.h"
#include "http/server/responses/response_400.h"
#include "http/server/responses/response_404.h"
#include "http/server/responses/response_500.h"
#include "http/server/routing/matches.h"
#include "http/server/routing/node.h"
#include "http/server/routing/routing.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"
#include "logging/logging.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP::Routing
{

	namespace
	{

		std::vector<std::unique_ptr<Interfaces::IWebRouteBase>> http_routes_vec{};
		std::vector<std::unique_ptr<Interfaces::IWebSocketBase>> ws_routes_vec{};

		HTTP::Routing::impl<Interfaces::IWebRouteBase> http_routes{};
		HTTP::Routing::impl<Interfaces::IWebSocketBase> ws_routes{};

		std::optional<StaticFileHandler> sf_route;

		SecurityConfig security_config{};

		// The active credential resolver (empty => everything is anonymous) and
		// the Subject of the request currently being dispatched.  The io_context
		// runs on a single thread and route handlers execute synchronously inside
		// HTTP_OnRequest, so one file-scope current subject is safe (and mirrors
		// how the rest of this translation unit already manages per-request state).
		SubjectResolver subject_resolver{};
		Auth::Subject current_subject{ Auth::Subject::Anonymous() };

		Auth::Subject ResolveSubject(const HTTP::Request& req, bool is_websocket_upgrade)
		{
			if (!security_config.AuthModeEnabled || !subject_resolver)
			{
				return Auth::Subject::Anonymous();
			}

			return subject_resolver(req, is_websocket_upgrade);
		}

		// Per-source-IP throttle for FAILED bearer-token attempts. The token compare
		// is already constant-time, but nothing slowed online guessing of a weak
		// token. After MAX_FAILURES failures from one IP, further attempts from it are
		// answered 429 for BAN_WINDOW; a successful auth (or a fresh policy) clears the
		// IP. The io_context runs on a single thread, so no locking is required.
		class AuthRateLimiter
		{
		public:
			[[nodiscard]] bool IsBanned(std::string_view ip) const
			{
				if (ip.empty()) { return false; }
				const auto it = m_Entries.find(std::string(ip));
				return (it != m_Entries.end()) && (std::chrono::steady_clock::now() < it->second.banned_until);
			}

			void RecordFailure(std::string_view ip)
			{
				if (ip.empty()) { return; }
				PruneIfLarge();
				auto& entry = m_Entries[std::string(ip)];
				if (++entry.failures >= MAX_FAILURES)
				{
					entry.banned_until = std::chrono::steady_clock::now() + BAN_WINDOW;
					entry.failures = 0;
					LogWarning(Channel::Web, [ip] { return std::format("Rate-limiting source '{}' for {}s after {} failed API authentication attempts",
						ip, std::chrono::duration_cast<std::chrono::seconds>(BAN_WINDOW).count(), MAX_FAILURES); });
				}
			}

			void RecordSuccess(std::string_view ip)
			{
				if (ip.empty()) { return; }
				m_Entries.erase(std::string(ip));
			}

			void Reset() { m_Entries.clear(); }

		private:
			struct Entry
			{
				unsigned int failures{ 0 };
				std::chrono::steady_clock::time_point banned_until{};
			};

			// Bound memory against many distinct source IPs: when the table grows
			// large, drop entries that are neither banned nor accumulating failures.
			void PruneIfLarge()
			{
				if (m_Entries.size() < MAX_TRACKED_IPS) { return; }
				const auto now = std::chrono::steady_clock::now();
				std::erase_if(m_Entries, [now](const auto& kv) { return kv.second.failures == 0 && kv.second.banned_until <= now; });
			}

			static constexpr unsigned int MAX_FAILURES = 10;
			static constexpr auto BAN_WINDOW = std::chrono::seconds(60);
			static constexpr std::size_t MAX_TRACKED_IPS = 8192;

			std::unordered_map<std::string, Entry> m_Entries;
		};

		AuthRateLimiter auth_rate_limiter{};

		// Attaching descriptive text to a profiling zone is a no-op in every
		// non-Tracy build (the base IProfilingUnit::Text() does nothing), so building
		// the std::format string unconditionally is pure waste on the request hot
		// path. Compile the formatting out unless a profiler backend is enabled; the
		// callable is only invoked when the text will actually be recorded.
		template<typename FN>
		void DeferredZoneText([[maybe_unused]] const Types::ProfilingUnitTypePtr& zone, [[maybe_unused]] FN&& make_text)
		{
#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
			if (zone)
			{
				zone->Text(std::forward<FN>(make_text)());
			}
#endif
		}

		// Constant-time comparison of two byte sequences. Always inspects the full
		// length of both operands so the running time does not leak how many leading
		// characters matched (mitigating a timing side-channel on the shared token).
		[[nodiscard]] bool ConstantTimeEquals(std::string_view lhs, std::string_view rhs) noexcept
		{
			// Seed the accumulator with a definite non-zero value when the lengths
			// differ (avoiding a low-bits-only fold that could collide), then OR in the
			// per-byte differences over the longer span so the loop never short-circuits.
			unsigned int diff = (lhs.size() == rhs.size()) ? 0U : 1U;

			const std::size_t n = std::max(lhs.size(), rhs.size());
			for (std::size_t i = 0; i < n; ++i)
			{
				const unsigned int a = (i < lhs.size()) ? static_cast<unsigned char>(lhs[i]) : 0U;
				const unsigned int b = (i < rhs.size()) ? static_cast<unsigned char>(rhs[i]) : 0U;
				diff |= (a ^ b);
			}

			return diff == 0U;
		}

		// Build a small text/plain error response (401/403) mirroring the existing
		// Response_4xx helpers. The body is intentionally generic so we never echo
		// the supplied (or expected) token back to the caller.
		[[nodiscard]] HTTP::Response MakeSecurityResponse(const HTTP::Request& req, HTTP::Status status, std::string_view body)
		{
			boost::beast::http::response<boost::beast::http::string_body> res{ status, req.version() };
			res.set(boost::beast::http::field::server, ServerFields::Server());
			res.set(boost::beast::http::field::content_type, ContentTypes::TEXT_PLAIN);
			if (status == HTTP::Status::unauthorized)
			{
				// RFC 7235: a 401 SHOULD carry a WWW-Authenticate challenge.
				res.set(boost::beast::http::field::www_authenticate, "Bearer");
			}
			res.keep_alive(req.keep_alive());
			res.body() = std::string(body);
			res.prepare_payload();
			return res;
		}

		// True for HTTP methods that mutate server state and therefore warrant the
		// optional CSRF custom-header requirement.
		[[nodiscard]] bool IsStateChangingMethod(boost::beast::http::verb method) noexcept
		{
			switch (method)
			{
			case boost::beast::http::verb::post:
			case boost::beast::http::verb::put:
			case boost::beast::http::verb::patch:
			case boost::beast::http::verb::delete_:
				return true;
			default:
				return false;
			}
		}

		// Beast's field value type (boost::beast::string_view) is not always the same
		// type as std::string_view; convert via data()/size() so this compiles on any
		// Boost version.
		template<typename SV>
		[[nodiscard]] std::string_view ToStringView(const SV& sv) noexcept
		{
			return std::string_view{ sv.data(), sv.size() };
		}

		// Look up a header value as a std::string_view, returning an empty view when
		// the header is absent.
		template<typename FIELD>
		[[nodiscard]] std::string_view HeaderValue(const HTTP::Request& req, FIELD field)
		{
			const auto it = req.find(field);
			return (it != req.end()) ? ToStringView(it->value()) : std::string_view{};
		}

		// Extract and constant-time-compare a bearer token offered via the
		// WebSocket handshake's Sec-WebSocket-Protocol header.  Browsers cannot set
		// an Authorization header on a WebSocket upgrade, so the UI offers the token
		// as a `bearer.<token>` subprotocol entry alongside the `aqualink` marker
		// (e.g. "aqualink, bearer.<token>").  NEVER logs the token.
		[[nodiscard]] bool WebSocketSubprotocolTokenMatches(const HTTP::Request& req, std::string_view expected_token)
		{
			const std::string_view header = HeaderValue(req, boost::beast::http::field::sec_websocket_protocol);
			static constexpr std::string_view BEARER_ENTRY_PREFIX{ "bearer." };

			std::size_t pos = 0;
			while (pos <= header.size())
			{
				const std::size_t comma = header.find(',', pos);
				std::string_view entry = (comma == std::string_view::npos)
					? header.substr(pos)
					: header.substr(pos, comma - pos);

				// Trim surrounding optional whitespace (per the header's ABNF).
				while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) { entry.remove_prefix(1); }
				while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) { entry.remove_suffix(1); }

				if (entry.starts_with(BEARER_ENTRY_PREFIX))
				{
					if (ConstantTimeEquals(entry.substr(BEARER_ENTRY_PREFIX.size()), expected_token))
					{
						return true;
					}
				}

				if (comma == std::string_view::npos) { break; }
				pos = comma + 1;
			}

			return false;
		}

		// Evaluate the active SecurityConfig against a parsed request. Returns the
		// rejection response to send when the request is denied, or std::nullopt when
		// it is permitted. Shared by the HTTP and WebSocket-upgrade paths so both
		// enforce identical rules. NEVER logs the configured or supplied token.
		[[nodiscard]] std::optional<HTTP::Response> EvaluateSecurity(const HTTP::Request& req, bool is_websocket_upgrade, std::string_view peer_ip)
		{
			const SecurityConfig& cfg = security_config;
			if (!cfg.IsEnabled())
			{
				// Feature fully disabled (the default) -> identical to historical behaviour.
				return std::nullopt;
			}

			// --- Origin allow-list (primary cross-site / cross-origin defence) ---
			if (!cfg.AllowedOrigins.empty())
			{
				const std::string_view origin = HeaderValue(req, boost::beast::http::field::origin);

				const bool origin_allowed = !origin.empty() &&
					std::any_of(cfg.AllowedOrigins.begin(), cfg.AllowedOrigins.end(),
						[origin](const std::string& allowed) { return allowed == origin; });

				if (!origin_allowed)
				{
					LogWarning(Channel::Web, [&] { return std::format("Rejected {} request: Origin '{}' is not in the allow-list", is_websocket_upgrade ? "WebSocket upgrade" : "HTTP", origin); });
					return MakeSecurityResponse(req, HTTP::Status::forbidden, "Forbidden: origin not allowed.");
				}
			}

			// --- Bearer token authentication (legacy shared token) ---
			// Superseded when the identity system is active: bearer credentials are
			// then interpreted by the subject resolver (JWT session / API key) and
			// enforcement happens in EvaluateAccess via the PolicyEngine instead.
			if (cfg.AuthToken.has_value() && !cfg.AuthModeEnabled)
			{
				// Brute-force throttle: a source that has failed auth too many times
				// recently is refused with 429 before the token is even examined.
				if (auth_rate_limiter.IsBanned(peer_ip))
				{
					LogWarning(Channel::Web, [&] { return std::format("Rejected {} request from rate-limited source '{}'", is_websocket_upgrade ? "WebSocket upgrade" : "HTTP", peer_ip); });
					auto throttled = MakeSecurityResponse(req, HTTP::Status::too_many_requests, "Too many failed authentication attempts; try again later.");
					throttled.set(boost::beast::http::field::retry_after, "60");
					return throttled;
				}

				static constexpr std::string_view BEARER_PREFIX{ "Bearer " };

				const std::string_view header = HeaderValue(req, boost::beast::http::field::authorization);

				bool authorised = false;
				if (header.starts_with(BEARER_PREFIX))
				{
					const std::string_view presented = header.substr(BEARER_PREFIX.size());
					authorised = ConstantTimeEquals(presented, *cfg.AuthToken);
				}

				// Browsers cannot attach an Authorization header to a WebSocket
				// upgrade, so for upgrades also accept the token carried in the
				// Sec-WebSocket-Protocol header as a `bearer.<token>` entry.
				if (!authorised && is_websocket_upgrade)
				{
					authorised = WebSocketSubprotocolTokenMatches(req, *cfg.AuthToken);
				}

				if (!authorised)
				{
					auth_rate_limiter.RecordFailure(peer_ip);
					LogWarning(Channel::Web, [&] { return std::format("Rejected unauthenticated {} request (missing/invalid bearer token)", is_websocket_upgrade ? "WebSocket upgrade" : "HTTP"); });
					return MakeSecurityResponse(req, HTTP::Status::unauthorized, "Unauthorized.");
				}

				// A genuine success clears any accumulated failures for this source.
				auth_rate_limiter.RecordSuccess(peer_ip);
			}

			// --- CSRF mitigation for state-changing requests ---
			// (Not applicable to the WebSocket upgrade, which is always a GET.)
			if (cfg.RequireCsrfHeader && !is_websocket_upgrade && IsStateChangingMethod(req.method()))
			{
				const std::string_view csrf = HeaderValue(req, boost::beast::string_view{ "X-Requested-With" });
				if (csrf.empty())
				{
					LogWarning(Channel::Web, [&] { return std::format("Rejected state-changing {} request: missing X-Requested-With header", magic_enum::enum_name(req.method())); });
					return MakeSecurityResponse(req, HTTP::Status::forbidden, "Forbidden: missing CSRF header.");
				}
			}

			return std::nullopt;
		}

		// True when `address` falls inside the "prefix/len" CIDR block.  A parse
		// failure of either side answers false (fail closed: no trust extended).
		[[nodiscard]] bool AddressInCidr(const boost::asio::ip::address& address, const std::string& cidr)
		{
			try
			{
				if (address.is_v4())
				{
					const auto network = boost::asio::ip::make_network_v4(cidr);
					return boost::asio::ip::network_v4(address.to_v4(), network.prefix_length()).canonical() == network.canonical();
				}

				const auto network = boost::asio::ip::make_network_v6(cidr);
				return boost::asio::ip::network_v6(address.to_v6(), network.prefix_length()).canonical() == network.canonical();
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		// PDP gate for a declared access requirement.  Deny maps to 401 for an
		// anonymous subject (a login could elevate) and 403 for an authenticated
		// one (logged in, not entitled).  Auth-mode disabled => posture makes the
		// decision Permit, so this stays a no-op for historical deployments.
		[[nodiscard]] std::optional<HTTP::Response> EvaluateAccess(const HTTP::Request& req, const Interfaces::AccessRequirement& requirement, std::string_view resource_id = {})
		{
			if (!security_config.AuthModeEnabled || !requirement.IsSpecified())
			{
				return std::nullopt;
			}

			const Auth::ResourceRef resource{ .Kind = std::string{ requirement.ResourceKind }, .Id = std::string{ resource_id } };
			const Auth::Environment environment{ .AuthEnabled = true };

			if (Auth::Decision::Permit == Auth::PolicyEngine::Decide(current_subject, requirement.Action, resource, environment))
			{
				return std::nullopt;
			}

			LogWarning(Channel::Web, [&] { return std::format("Denied {} {} for subject '{}' (missing entitlement '{}')", magic_enum::enum_name(req.method()), std::string_view(req.target()), current_subject.Id, requirement.Action); });

			if (!current_subject.Authenticated)
			{
				return MakeSecurityResponse(req, HTTP::Status::unauthorized, "Unauthorized.");
			}

			return MakeSecurityResponse(req, HTTP::Status::forbidden, "Forbidden: not entitled.");
		}

	}
	// unnamed namespace

	void Clear()
	{
		http_routes_vec.clear();
		ws_routes_vec.clear();
		http_routes = HTTP::Routing::impl<Interfaces::IWebRouteBase>{};
		ws_routes = HTTP::Routing::impl<Interfaces::IWebSocketBase>{};
		sf_route.reset();
		security_config = SecurityConfig{};
		auth_rate_limiter.Reset();
		subject_resolver = SubjectResolver{};
		current_subject = Auth::Subject::Anonymous();
	}

	void SetSubjectResolver(SubjectResolver resolver)
	{
		subject_resolver = std::move(resolver);
	}

	std::string EffectiveClientIp(const HTTP::Request& req, std::string_view peer_ip)
	{
		const auto& cidrs = security_config.TrustedProxyCidrs;

		if (cidrs.empty() || peer_ip.empty())
		{
			return std::string{ peer_ip };
		}

		boost::system::error_code parse_ec;
		const auto peer = boost::asio::ip::make_address(std::string{ peer_ip }, parse_ec);

		if (parse_ec)
		{
			return std::string{ peer_ip };
		}

		const bool trusted = std::any_of(cidrs.begin(), cidrs.end(), [&](const auto& cidr) { return AddressInCidr(peer, cidr); });

		if (!trusted)
		{
			// XFF from an untrusted source is attacker-controlled: ignore it.
			return std::string{ peer_ip };
		}

		const std::string_view xff = HeaderValue(req, boost::beast::string_view{ "X-Forwarded-For" });

		if (xff.empty())
		{
			return std::string{ peer_ip };
		}

		// First entry == the original client (each proxy appends).
		auto first = xff.substr(0, xff.find(','));
		while (!first.empty() && ((' ' == first.front()) || ('\t' == first.front()))) { first.remove_prefix(1); }
		while (!first.empty() && ((' ' == first.back()) || ('\t' == first.back()))) { first.remove_suffix(1); }

		return first.empty() ? std::string{ peer_ip } : std::string{ first };
	}

	const Auth::Subject& CurrentSubject()
	{
		return current_subject;
	}

	void SetSecurityConfig(SecurityConfig config)
	{
		security_config = std::move(config);
		auth_rate_limiter.Reset();

		if (security_config.IsEnabled())
		{
			LogInfo(Channel::Web, [&]
				{
					return std::format("HTTP/WS security policy enabled (auth={}, origin_allowlist={}, csrf_header={})",
						security_config.AuthToken.has_value(),
						security_config.AllowedOrigins.size(),
						security_config.RequireCsrfHeader);
				});
		}
	}

	const SecurityConfig& GetSecurityConfig()
	{
		return security_config;
	}

	void Add(std::unique_ptr<Interfaces::IWebRouteBase>&& handler)
	{
		auto& handler_ref = http_routes_vec.emplace_back(std::move(handler));

		LogTrace(Channel::Web, [&] { return std::format("Adding HTTP handler for route '{}'", handler_ref->Route()); });

		http_routes.insert_impl(handler_ref->Route(), handler_ref.get());
	}

	void Add(std::unique_ptr<Interfaces::IWebSocketBase>&& handler)
	{
		auto& handler_ref = ws_routes_vec.emplace_back(std::move(handler));

		LogTrace(Channel::Web, [&] { return std::format("Adding WebSocket handler for route '{}'", handler_ref->Route()); });

		ws_routes.insert_impl(handler_ref->Route(), handler_ref.get());
	}

	void StaticHandler(StaticFileHandler&& sf)
	{
		LogTrace(Channel::Web, "Adding static file handler");
		sf_route = std::move(sf);
	}

	HTTP::Message HTTP_OnRequest(const HTTP::Request& req, std::string_view raw_peer_ip)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Routing::RouteRequest", std::source_location::current());
		DeferredZoneText(zone, [&] { return std::format("{} {}", magic_enum::enum_name(req.method()), std::string_view(req.target())); });

		// Trusted-proxy aware client identity: behind a reverse proxy every
		// client shares the proxy's peer address, which would let one guest's
		// auth failures rate-limit everyone (and an attacker hide in the crowd).
		const std::string effective_ip = EffectiveClientIp(req, raw_peer_ip);
		const std::string_view peer_ip{ effective_ip };

		try
		{
			// Security is enforced PER-BRANCH below rather than up front: registered
			// routes (incl. /api and /metrics) and unmatched paths are gated, but
			// static assets are served WITHOUT authentication so a token-protected
			// deployment can still load index.html / scripts / css to render the
			// login screen.  A registered route may additionally opt OUT of the policy
			// via IWebRouteBase::RequiresAuthentication() (the /api/health probe does).
			// When the policy is disabled (the default) EvaluateSecurity is a cheap
			// no-op, so behaviour is byte-identical to before.
			std::filesystem::path static_file_result;
			HTTP::Routing::matches m;

			std::string_view* matches_it = m.matches();
			std::string_view* ids_it = m.ids();
			std::string_view* matches_end = m.matches() + m.size();
			std::string_view* ids_end = m.ids() + m.size();

			// Parse the request target as an origin-form URL (path [ "?" query ]) rather
			// than as a bare path. parse_path requires the WHOLE input to be a valid path,
			// so any request carrying a query string (e.g. /api/history/series?key=...)
			// would fail to parse and be rejected 400 before ever reaching its handler.
			// Routing only matches on the path, so the query is parsed and then ignored.
			if (auto target_url = boost::urls::parse_origin_form(req.target()); target_url.has_error())
			{
				Factory::ProfilerFactory::Instance().Get()->Message("HTTP 400 Bad Request");
				LogDebug(Channel::Web, [&] { return std::format("Supplied http target could not be parsed; error was -> {}", target_url.error().message()); });
				return HTTP::Responses::Response_400(req);
			}
			else if (auto p = http_routes.find_impl(target_url->encoded_segments(), matches_it, ids_it, matches_end, ids_end); nullptr != p)
			{
				// Registered route -> enforce security before dispatch, unless the
				// route opts out (e.g. the unauthenticated /api/health liveness probe,
				// which an orchestrator must reach without the operator's bearer token).
				if (p->RequiresAuthentication())
				{
					if (auto rejection = EvaluateSecurity(req, false, peer_ip); rejection.has_value())
					{
						return std::move(*rejection);
					}
				}

				// Resolve the request's subject (anonymous when auth-mode is off or
				// no credentials are presented) and gate the route's declared access
				// through the PolicyEngine.
				current_subject = ResolveSubject(req, false);

				const auto requirement = p->RequiredAccess(req.method());

				// When the route declares per-resource grain (ResourceKind set), its
				// sole path parameter IS the resource id (e.g. /api/equipment/
				// buttons/{button_id}); the router matched it into m, so selector-
				// scoped entitlements (equipment.control.aux:<id>) are enforced HERE,
				// not left to the handler.
				const std::string_view access_resource_id = (!requirement.ResourceKind.empty()) ? *m.matches() : std::string_view{};

				if (auto denial = EvaluateAccess(req, requirement, access_resource_id); denial.has_value())
				{
					return std::move(*denial);
				}

				LogTrace(Channel::Web, [&] { return std::format("Handling HTTP {} request for {}", magic_enum::enum_name(req.method()), std::string_view(req.target())); });

				// Dynamic route responses (API equipment state, diagnostics, etc.)
				// reflect live data and must never be reused from a cache. The service
				// worker already treats /api as network-only; stamping no-store here —
				// the single place a registered route's mutable Response is still in
				// hand before it is type-erased into a message_generator — is the
				// defence-in-depth for direct browser fetches and any intermediary,
				// and means no handler has to remember to set it.
				HTTP::Response resp = p->OnRequest(req);
				resp.set(boost::beast::http::field::cache_control, "no-store");
				return resp;
			}
			else if (sf_route.has_value() && sf_route->match(req.target(), static_file_result))
			{
				// Static asset -> intentionally UNauthenticated (see above).
				LogTrace(Channel::Web, [&] { return std::format("Attempting to serve static content; file is -> {}", static_file_result.string()); });
				return HTTP::Responses::Response_StaticFile(req, static_file_result);
			}
			else
			{
				// Unmatched path -> still enforce security so an unknown /api/* path
				// answers 401 (not 404) when a token is required.
				if (auto rejection = EvaluateSecurity(req, false, peer_ip); rejection.has_value())
				{
					return std::move(*rejection);
				}

				// Same non-leak rule under the identity system: an unauthenticated
				// subject probing an unknown /api/* path gets 401 (not 404) so the
				// route surface is not enumerable without credentials.
				if (security_config.AuthModeEnabled)
				{
					current_subject = ResolveSubject(req, false);

					if (!current_subject.Authenticated && ToStringView(req.target()).starts_with("/api/"))
					{
						return MakeSecurityResponse(req, HTTP::Status::unauthorized, "Unauthorized.");
					}
				}

				Factory::ProfilerFactory::Instance().Get()->Message("HTTP 404 Not Found");
				LogDebug(Channel::Web, [&] { return std::format("Path '{}' was requested but no HTTP handler was available", std::string_view(req.target())); });
				LogDebug(Channel::Web, "Could not handle request -> returning a 404 NOT FOUND");
				return HTTP::Responses::Response_404(req);
			}
		}
		catch (const std::exception& ex)
		{
			// The detail stays server-side only; the client receives a context-free 500.
			// Promote to Warning so an escaping exception is visible at the default log
			// level (it indicates a route handler fault, not routine traffic).
			LogWarning(Channel::Web, [&] { return std::format("An exception was thrown while processing an HTTP request: exception was -> {}", ex.what()); });
			return HTTP::Responses::Response_500(req);
		}
	}

	Interfaces::IWebSocketBase* WS_OnAccept(const std::string_view target)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Routing::RouteWebSocket", std::source_location::current());
		DeferredZoneText(zone, [&] { return std::string(target); });

		try
		{
			HTTP::Routing::matches m;

			std::string_view* matches_it = m.matches();
			std::string_view* ids_it = m.ids();
			std::string_view* matches_end = m.matches() + m.size();
			std::string_view* ids_end = m.ids() + m.size();

			// Parse as origin-form (path [ "?" query ]) so a WebSocket upgrade target that
			// carries a query string still routes on its path (see HTTP_OnRequest above).
			if (auto target_url = boost::urls::parse_origin_form(target); target_url.has_error())
			{
				LogDebug(Channel::Web, [&] { return std::format("Supplied websocket target could not be parsed; error was -> {}", target_url.error().message()); });
			}
			else if (auto p = ws_routes.find_impl(target_url->encoded_segments(), matches_it, ids_it, matches_end, ids_end); nullptr == p)
			{
				LogDebug(Channel::Web, [&] { return std::format("Path '{}' was requested but no WS handler was available", target); });
			}
			else
			{
				LogTrace(Channel::Web, [&] { return std::format("Handling WS request for {}", target); });
				return p;
			}
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Web, [&] { return std::format("An exception was thrown while processing a WS request: exception was -> {}", ex.what()); });
		}

		LogDebug(Channel::Web, "Could not handle WS request -> returning nullptr");
		return nullptr;
	}

	std::optional<HTTP::Response> AuthorizeWebSocketUpgrade(const HTTP::Request& req, std::string_view raw_peer_ip)
	{
		const std::string effective_ip = EffectiveClientIp(req, raw_peer_ip);
		const std::string_view peer_ip{ effective_ip };

		if (auto rejection = EvaluateSecurity(req, true, peer_ip); rejection.has_value())
		{
			return rejection;
		}

		// Under the identity system the upgrade is additionally gated by the
		// socket's declared entitlement (equipment.view for the live-data feeds).
		if (security_config.AuthModeEnabled)
		{
			current_subject = ResolveSubject(req, true);

			if (auto* ws = WS_OnAccept(ToStringView(req.target())); nullptr != ws)
			{
				if (auto denial = EvaluateAccess(req, ws->RequiredAccess()); denial.has_value())
				{
					return denial;
				}
			}
		}

		return std::nullopt;
	}

}
// namespace AqualinkAutomate::HTTP::Routing
