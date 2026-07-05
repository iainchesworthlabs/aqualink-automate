#include <chrono>
#include <exception>
#include <format>
#include <string_view>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url_view.hpp>

#include <nlohmann/json.hpp>

#include "alerting/webhook_sink.h"
#include "alerting/webhook_tls.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace AqualinkAutomate::Alerting
{
	namespace
	{
		constexpr std::chrono::seconds WEBHOOK_TIMEOUT{ 10 };

		http::request<http::string_body> BuildRequest(std::string_view host, std::string_view target, std::string_view body)
		{
			http::request<http::string_body> req{ http::verb::post, target.empty() ? "/" : target, 11 };
			req.set(http::field::host, host);
			req.set(http::field::user_agent, "aqualink-automate");
			req.set(http::field::content_type, "application/json");
			req.body() = body;
			req.prepare_payload();
			return req;
		}

		// One HTTP attempt over a plain TCP stream.  Throws on any failure.
		asio::awaitable<void> AttemptHttp(std::string host, std::string port, std::string target, std::string body)
		{
			auto executor = co_await asio::this_coro::executor;
			tcp::resolver resolver(executor);
			beast::tcp_stream stream(executor);

			auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
			stream.expires_after(WEBHOOK_TIMEOUT);
			co_await stream.async_connect(results, asio::use_awaitable);

			auto req = BuildRequest(host, target, body);
			stream.expires_after(WEBHOOK_TIMEOUT);
			co_await http::async_write(stream, req, asio::use_awaitable);

			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			co_await http::async_read(stream, buffer, res, asio::use_awaitable);

			beast::error_code ec;
			stream.socket().shutdown(tcp::socket::shutdown_both, ec);
		}

		// One HTTPS attempt over a TLS stream.  Throws on any failure.
		asio::awaitable<void> AttemptHttps(std::string host, std::string port, std::string target, std::string body)
		{
			auto executor = co_await asio::this_coro::executor;
			tcp::resolver resolver(executor);

			ssl::context ctx(ssl::context::tls_client);

			// Authenticate the server certificate.  Without this a freshly constructed
			// Boost.Asio ssl::context defaults to verify_none, so the handshake would
			// succeed against ANY certificate (self-signed, expired, wrong-host) and an
			// on-path attacker could transparently MITM the webhook -- "https://" would
			// give encryption but no authentication.  ApplyClientTlsVerification loads
			// the trust store and sets verify_peer; the per-stream hostname check below
			// mirrors the MQTT client (mqtt_client.cpp).
			ApplyClientTlsVerification(ctx);

			beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);
			stream.set_verify_callback(ssl::host_name_verification(host));

			// SNI is mandatory for most modern TLS servers.
			if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
			{
				throw beast::system_error{ beast::error_code{ static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category() } };
			}

			auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
			beast::get_lowest_layer(stream).expires_after(WEBHOOK_TIMEOUT);
			co_await beast::get_lowest_layer(stream).async_connect(results, asio::use_awaitable);

			co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

			auto req = BuildRequest(host, target, body);
			beast::get_lowest_layer(stream).expires_after(WEBHOOK_TIMEOUT);
			co_await http::async_write(stream, req, asio::use_awaitable);

			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			co_await http::async_read(stream, buffer, res, asio::use_awaitable);

			beast::error_code ec;
			co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
		}

		// Run the POST with a single retry; on final failure, log and drop so the
		// main loop is never affected by an unreachable webhook.
		asio::awaitable<void> RunWithRetry(bool tls, std::string host, std::string port, std::string target, std::string body)
		{
			for (int attempt = 1; attempt <= 2; ++attempt)
			{
				try
				{
					if (tls)
					{
						co_await AttemptHttps(host, port, target, body);
					}
					else
					{
						co_await AttemptHttp(host, port, target, body);
					}
					co_return;
				}
				catch (const std::exception& ex)
				{
					if (attempt == 2)
					{
						LogWarning(Channel::Web, [host, ex_what = std::string{ ex.what() }] { return std::format("Alert webhook POST to {} failed (dropped): {}", host, ex_what); });
					}
				}
			}
			co_return;
		}
	}
	// unnamed namespace

	namespace
	{
		// Parse an http/https URL into connection components. Returns false (and
		// leaves the out-params unspecified) for an empty or non-http(s) URL.
		bool ParseWebhookUrl(const std::string& url, bool& use_tls, std::string& host, std::string& port, std::string& target)
		{
			if (url.empty()) { return false; }

			auto parsed = boost::urls::parse_uri(url);
			if (!parsed) { return false; }

			const auto scheme = parsed->scheme();
			if (scheme != "http" && scheme != "https") { return false; }

			use_tls = (scheme == "https");
			host = parsed->host();
			if (host.empty()) { return false; }

			port = parsed->has_port() ? std::string{ parsed->port() } : (use_tls ? "443" : "80");

			std::string t{ parsed->path() };
			if (t.empty()) { t = "/"; }
			if (parsed->has_query()) { t += "?"; t += parsed->query(); }
			target = std::move(t);
			return true;
		}
	}
	// unnamed namespace

	WebhookSink::WebhookSink(boost::asio::io_context& io_context, UrlProvider url_provider) :
		m_IoContext(io_context),
		m_UrlProvider(std::move(url_provider))
	{
	}

	std::string WebhookSink::BuildPayload(const AlertTransition& transition)
	{
		nlohmann::json payload;
		payload["condition"] = transition.condition;
		payload["state"] = transition.raised ? "raised" : "cleared";
		payload["ts"] = transition.ts;
		payload["detail"] = transition.detail;

		// Structured values behind the detail prose (additive; omitted when the
		// transition carries none) so automations can act on the data without
		// parsing English text.
		if (transition.params.is_object() && !transition.params.empty())
		{
			payload["params"] = transition.params;
		}

		return payload.dump();
	}

	void WebhookSink::Post(const AlertTransition& transition)
	{
		if (!m_UrlProvider)
		{
			return;
		}

		// Read the URL fresh so a runtime preference change takes effect.
		bool use_tls = false;
		std::string host;
		std::string port;
		std::string target;
		if (!ParseWebhookUrl(m_UrlProvider(), use_tls, host, port, target))
		{
			return;
		}

		boost::asio::co_spawn(
			m_IoContext,
			RunWithRetry(use_tls, host, port, target, BuildPayload(transition)),
			boost::asio::detached);
	}

}
// namespace AqualinkAutomate::Alerting
