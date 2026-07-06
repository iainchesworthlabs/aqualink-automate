#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_types.h"
#include "http/webroute_diagnostics_matter.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		constexpr std::chrono::milliseconds SIDECAR_TIMEOUT{ 750 };

		// Coroutine body of the sidecar fetch, extracted from the co_spawn lambda so the
		// lambda stays short. `body` is the caller's local (its stack frame outlives this
		// coroutine because ioc.run_for completes it before returning), and status_port is
		// passed by value exactly as the lambda captured it.
		asio::awaitable<void> FetchSidecarStatusCoro(uint16_t status_port, std::optional<std::string>& body)
		{
			auto executor = co_await asio::this_coro::executor;
			tcp::resolver resolver(executor);
			beast::tcp_stream stream(executor);

			// Use as_tuple so failures (a sidecar that isn't running -> refused
			// connect, etc.) come back as error_codes instead of THROWING
			// boost::system::system_error -- otherwise every poll of an absent
			// sidecar raised (and caught) a first-chance exception.
			auto [resolve_ec, results] = co_await resolver.async_resolve("127.0.0.1", std::to_string(status_port), asio::as_tuple(asio::use_awaitable));
			if (resolve_ec) { co_return; }

			stream.expires_after(SIDECAR_TIMEOUT);
			auto [connect_ec, connected_endpoint] = co_await stream.async_connect(results, asio::as_tuple(asio::use_awaitable));
			if (connect_ec) { co_return; }
			(void)connected_endpoint;

			http::request<http::string_body> req{ http::verb::get, "/matter/status", 11 };
			req.set(http::field::host, "127.0.0.1");
			req.set(http::field::user_agent, "aqualink-automate");
			stream.expires_after(SIDECAR_TIMEOUT);
			auto [write_ec, bytes_written] = co_await http::async_write(stream, req, asio::as_tuple(asio::use_awaitable));
			if (write_ec) { co_return; }
			(void)bytes_written;

			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			auto [read_ec, bytes_read] = co_await http::async_read(stream, buffer, res, asio::as_tuple(asio::use_awaitable));
			if (read_ec) { co_return; }
			(void)bytes_read;
			body = res.body();

			beast::error_code ec;
			stream.socket().shutdown(tcp::socket::shutdown_both, ec);
		}

		// Fetch the sidecar's /matter/status over localhost on a throwaway io_context,
		// bounded so a hung/absent sidecar fails fast. Called ONLY from the route's
		// background RefreshLoop thread -- never the main io_context -- so the bounded
		// run_for here can block this thread without ever stalling the serial reader.
		// Returns the response body, or nullopt if it could not be reached in time.
		std::optional<std::string> FetchSidecarStatus(uint16_t status_port)
		{
			try
			{
				asio::io_context ioc;
				std::optional<std::string> body;

				asio::co_spawn(
					ioc,
					FetchSidecarStatusCoro(status_port, body),
					asio::detached);

				// Bound the whole exchange; a refused localhost connection fails fast.
				ioc.run_for(SIDECAR_TIMEOUT + std::chrono::milliseconds(250));
				return body;
			}
			catch (const std::exception&)   // NOSONAR(cpp:S1181) — boundary: runs on the background RefreshLoop thread and must degrade to "unreachable" (nullopt) on ANY failure rather than let an exception escape and kill the poller thread
			{
				return std::nullopt;
			}
		}
	}
	// unnamed namespace

	WebRoute_Diagnostics_Matter::WebRoute_Diagnostics_Matter(bool enabled, uint16_t status_port) :
		m_Enabled(enabled),
		m_StatusPort(status_port)
	{
		// Only spin up the background poller when Matter is actually enabled; a disabled
		// route answers purely from m_Enabled and never touches the sidecar.
		if (m_Enabled)
		{
			m_RefreshThread = std::thread([this]() { RefreshLoop(); });
		}
	}

	WebRoute_Diagnostics_Matter::~WebRoute_Diagnostics_Matter()
	{
		{
			std::scoped_lock lock(m_WakeMutex);
			m_Stop = true;
		}
		m_WakeCv.notify_all();

		if (m_RefreshThread.joinable())
		{
			m_RefreshThread.join();
		}
	}

	void WebRoute_Diagnostics_Matter::RefreshLoop()
	{
		// How often the cached snapshot is refreshed. Each poll runs entirely on THIS
		// thread (never the main io_context), so a slow/absent sidecar only stalls the
		// poller, never the serial reader.
		constexpr std::chrono::seconds REFRESH_INTERVAL{ 5 };

		for (;;)
		{
			auto body = FetchSidecarStatus(m_StatusPort);
			{
				std::scoped_lock lock(m_StatusMutex);
				m_CachedStatusBody = std::move(body);
			}

			std::unique_lock wake_lock(m_WakeMutex);
			m_WakeCv.wait_for(wake_lock, REFRESH_INTERVAL, [this]() { return m_Stop; });
			if (m_Stop)
			{
				return;
			}
		}
	}

	HTTP::Response WebRoute_Diagnostics_Matter::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Matter::OnRequest", std::source_location::current());

		if (req.method() != HTTP::Verbs::get)
		{
			return HTTP::Responses::Response_405(req);
		}

		nlohmann::json json;
		json["enabled"] = m_Enabled;

		if (!m_Enabled)
		{
			return MakeJsonResponse(req, HTTP::Status::ok, json.dump());
		}

		json["status_port"] = m_StatusPort;

		// Read the most recent background-refreshed snapshot. This NEVER blocks the main
		// loop: the actual (potentially slow) sidecar fetch happens on RefreshLoop's thread.
		std::optional<std::string> status_body;
		{
			std::scoped_lock lock(m_StatusMutex);
			status_body = m_CachedStatusBody;
		}

		if (!status_body.has_value())
		{
			// Enabled but the sidecar is not answering (still starting, or crashed).
			json["reachable"] = false;
			json["running"] = false;
			json["paired"] = false;
			return MakeJsonResponse(req, HTTP::Status::ok, json.dump());
		}

		json["reachable"] = true;

		// Merge the sidecar's status object ({ running, paired, fabrics, qr_payload,
		// manual_code }) into the response. Tolerate a malformed body defensively.
		if (auto parsed = nlohmann::json::parse(status_body.value(), nullptr, false); !parsed.is_discarded() && parsed.is_object())
		{
			for (auto it = parsed.begin(); it != parsed.end(); ++it)
			{
				json[it.key()] = it.value();
			}
		}

		return MakeJsonResponse(req, HTTP::Status::ok, json.dump());
	}

}
// namespace AqualinkAutomate::HTTP
