#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "application/application_defaults.h"
#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Web
{

	typedef struct
	{
		std::filesystem::path certificate;
		std::filesystem::path private_key;
	}
	SslCertificate;

	typedef struct tagWebSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Web" };
			return AREA_NAME;
		}

		tagWebSettings() :
			http_port{ 80 },
			https_port{ 443 },
			http_content_is_disabled{ false },
			http_server_is_enabled{ true },
			https_server_is_enabled{ true }
		{
		}

		std::string bind_address;
		uint16_t http_port;
		uint16_t https_port;

		std::string doc_root;

		bool http_content_is_disabled;
		bool http_server_is_enabled;
		bool https_server_is_enabled;

		SslCertificate ssl_certificate;
		std::optional<std::filesystem::path> ca_chain_certificate;

		// Optional API bearer token (opt-in, default UNSET => no auth, exactly as
		// before). When set, the HTTP server requires every API request (and the
		// WebSocket upgrade) to carry "Authorization: Bearer <token>"; a missing or
		// mismatched token is answered with HTTP 401. Enforcement lives in the
		// http-server-routing layer (constant-time compare; the token is never
		// logged).
		std::optional<std::string> ApiAuthToken;

		// Optional cross-site protections, off by default (backward-compatible).
		// AllowedOrigins, when non-empty, makes the routing layer reject any API
		// request / WebSocket upgrade whose Origin header is not on the list (403).
		// ApiRequireCsrfHeader, when true, makes state-changing methods require the
		// X-Requested-With header. Both were implemented in the routing layer but had
		// no option to enable them until now.
		std::vector<std::string> ApiAllowedOrigins;
		bool ApiRequireCsrfHeader{ false };

		// Operator acknowledgement that the API is intentionally exposed on a
		// non-loopback address without authentication (e.g. behind a trusted reverse
		// proxy). Does NOT change behaviour; it only downgrades the prominent
		// open-control-plane startup warning to an informational note.
		bool InsecureNoAuthAck{ false };
	}
	WebSettings;

	class OptionsProcessor
	{
	private:
		// Default to localhost for security - use --address 0.0.0.0 to expose on all interfaces
		AppOptionPtr OPTION_INTERFACE{ make_appoption("address", "Specific network interface to which to bind", boost::program_options::value<std::string>()->default_value("127.0.0.1")) };
		AppOptionPtr OPTION_HTTPPORT{ make_appoption("http-port", "Specific HTTP port number on which to listen", boost::program_options::value<uint16_t>()) };
		AppOptionPtr OPTION_HTTPSPORT{ make_appoption("https-port", "Specific HTTPS port number on which to listen", boost::program_options::value<uint16_t>()) };
		AppOptionPtr OPTION_NOCONTENT{ make_appoption("disable-content", "Disable serving of rendered content; only enable APIs", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_NOHTTP{ make_appoption("disable-http", "Disable HTTP support for web services", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_NOHTTPS{ make_appoption("disable-https", "Disable HTTPS support for web services", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_TLSCERT{ make_appoption("cert", "Specify the certificate (PEM format) to use", boost::program_options::value<std::string>()->default_value(Application::DEFAULT_CERTIFICATE)) };
		AppOptionPtr OPTION_TLSCERTKEY{ make_appoption("cert-key", "Specify the certificate's key (PEM format) to use", boost::program_options::value<std::string>()->default_value(Application::DEFAULT_PRIVATE_KEY)) };
		AppOptionPtr OPTION_TLSCACERT{ make_appoption("cachain-cert", "Specify the CA chain certificate (PEM format) to use", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_DOCROOT{ make_appoption("doc-root", "The location from which HTML files are served", boost::program_options::value<std::string>()->default_value(Application::DOC_ROOT)) };
		// Opt-in API auth token; UNSET by default (no auth, backward-compatible). No default_value
		// so an absent flag leaves the option unset rather than producing an empty-string token.
		AppOptionPtr OPTION_APIAUTHTOKEN{ make_appoption("api-auth-token", "Require 'Authorization: Bearer <token>' on API requests and the WebSocket upgrade (default: no auth)", boost::program_options::value<std::string>()) };
		// Repeatable (composing) Origin allow-list. No default_value, so an absent flag leaves the list empty (check disabled).
		AppOptionPtr OPTION_APIALLOWEDORIGIN{ make_appoption("api-allowed-origin", "Allowed Origin for the API/WebSocket (repeatable); when set, a request/upgrade whose Origin is not listed is rejected with HTTP 403", boost::program_options::value<std::vector<std::string>>()->composing()) };
		AppOptionPtr OPTION_APIREQUIRECSRF{ make_appoption("api-require-csrf-header", "Require the 'X-Requested-With' header on state-changing API requests (CSRF mitigation)", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_INSECURENOAUTH{ make_appoption("insecure-no-auth", "Acknowledge an intentionally unauthenticated API on a non-loopback bind (silences the open-control-plane startup warning)", boost::program_options::bool_switch()->default_value(false)) };

		const std::vector<AppOptionPtr> WebOptionsCollection
		{
			OPTION_INTERFACE,
			OPTION_HTTPPORT,
			OPTION_HTTPSPORT,
			OPTION_NOCONTENT,
			OPTION_NOHTTP,
			OPTION_NOHTTPS,
			OPTION_TLSCERT,
			OPTION_TLSCERTKEY,
			OPTION_TLSCACERT,
			OPTION_DOCROOT,
			OPTION_APIAUTHTOKEN,
			OPTION_APIALLOWEDORIGIN,
			OPTION_APIREQUIRECSRF,
			OPTION_INSECURENOAUTH
		};

	public:
		using SettingsType = WebSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Web
