#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include "formatters/beast_stringview_formatter.h"
#include "http/server/http_server.h"
#include "http/server/routing/routing.h"
#include "http/server/responses/response_404.h"
#include "logging/logging.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "profiling/profiling_units/unit_colours.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	//=========================================================================
	// HttpSessionState
	//=========================================================================

	HttpSessionState::HttpSessionState(TcpSocket socket, std::string peer_ip)
		: m_PeerIp(std::move(peer_ip))
	{
		m_TcpStream.emplace(std::move(socket));
		m_TcpStream->expires_after(SESSION_TIMEOUT);
	}

	HttpSessionState::HttpSessionState(TcpSocket socket, boost::asio::ssl::context& ssl_ctx, std::string peer_ip)
		: m_HasSslContext(true)
		, m_SslContext(&ssl_ctx)
		, m_PeerIp(std::move(peer_ip))
	{
		m_TcpStream.emplace(std::move(socket));
		m_TcpStream->expires_after(SESSION_TIMEOUT);
	}

	void HttpSessionState::Start()
	{
		if (m_HasSslContext && m_SslContext)
		{
			DoSslHandshake();
		}
		else
		{
			DoRead();
		}
	}

	void HttpSessionState::Poll()
	{
		if (m_Done) return;

		// Periodically re-validate the live connection's credential: a token
		// that has expired or been revoked (tokver bump / account disable /
		// entitlement loss) closes the socket, so the client reconnects with a
		// fresh credential.  Throttled — token state changes on the order of
		// seconds, not frames.  No revalidator (auth-mode off) => never closes.
		if (m_WsActive && m_WsRevalidator && (++m_WsRevalidateTick >= WS_REVALIDATE_INTERVAL))
		{
			m_WsRevalidateTick = 0;

			if (!m_WsRevalidator())
			{
				LogInfo(Channel::Web, [&] { return std::format("Closing WebSocket connection {}: credential no longer authorised", m_WsConnectionId); });

				if (m_WsHandler)
				{
					m_WsHandler->OnClose(m_WsConnectionId);
				}

				Close();
				return;
			}
		}

		if (m_WsActive && !m_WsWriting)
		{
			TryWsWrite();
		}
	}

	bool HttpSessionState::IsDone() const
	{
		return m_Done;
	}

	void HttpSessionState::Close()
	{
		if (m_Done) return;

		MarkDone();

		if (m_WsSslStream)
		{
			boost::beast::get_lowest_layer(*m_WsSslStream).cancel();
		}
		else if (m_WsStream)
		{
			boost::beast::get_lowest_layer(*m_WsStream).cancel();
		}
		else if (m_SslStream)
		{
			boost::beast::get_lowest_layer(*m_SslStream).cancel();
		}
		else if (m_TcpStream)
		{
			boost::beast::get_lowest_layer(*m_TcpStream).cancel();
		}
	}

	//--- SSL Handshake ---------------------------------------------------

	void HttpSessionState::DoSslHandshake()
	{
		if (!m_TcpStream || !m_SslContext)
		{
			MarkDone();
			return;
		}

		m_SslStream.emplace(std::move(*m_TcpStream), *m_SslContext);
		m_TcpStream.reset();

		boost::beast::get_lowest_layer(*m_SslStream).expires_after(SESSION_TIMEOUT);

		m_SslStream->async_handshake(
			boost::asio::ssl::stream_base::server,
			[self = shared_from_this()](boost::system::error_code ec)
			{
				self->OnSslHandshake(ec);
			});
	}

	void HttpSessionState::OnSslHandshake(boost::system::error_code ec)
	{
		if (m_Done) return;

		if (ec)
		{
			// Promote to Warning: a failed TLS handshake at the default log level is an
			// actionable connectivity/cert problem, not routine traffic.
			LogWarning(Channel::Web, [&] { return std::format("SSL handshake failed: {}", ec.message()); });
			MarkDone();
			return;
		}

		DoRead();
	}

	//--- HTTP Read --------------------------------------------------------

	void HttpSessionState::ArmTimeout()
	{
		if (m_SslStream)
		{
			boost::beast::get_lowest_layer(*m_SslStream).expires_after(SESSION_TIMEOUT);
		}
		else if (m_TcpStream)
		{
			m_TcpStream->expires_after(SESSION_TIMEOUT);
		}
	}

	void HttpSessionState::DoRead()
	{
		if (m_Done) return;

		m_Parser.emplace();
		m_Parser->body_limit(10000);

		ArmTimeout();

		const bool dispatched = WithHttpStream([self = shared_from_this(), this](auto& stream)
			{
				boost::beast::http::async_read(stream, m_Buffer, *m_Parser,
					[self](boost::system::error_code ec, std::size_t bytes)
					{
						self->OnRead(ec, bytes);
					});
			});

		if (!dispatched)
		{
			MarkDone();
		}
	}

	void HttpSessionState::OnRead(boost::system::error_code ec, std::size_t /*bytes_transferred*/)
	{
		if (m_Done) return;

		if (ec == boost::beast::http::error::end_of_stream)
		{
			DoClose();
			return;
		}

		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				LogTrace(Channel::Web, [&] { return std::format("HTTP read failed: {}", ec.message()); });
			}
			MarkDone();
			return;
		}

		ProcessRequest();
	}

	//--- Process Request --------------------------------------------------

	void HttpSessionState::ProcessRequest()
	{
		if (!m_Parser)
		{
			MarkDone();
			return;
		}

		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("HttpServer::handle_request", std::source_location::current());

		auto req = m_Parser->release();

#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
		Factory::ProfilerFactory::Instance().Get()->Message(
			std::format("HTTP: {} {}", req.method_string(), req.target()),
			static_cast<uint32_t>(Profiling::UnitColours::Cyan));
#endif

		if (boost::beast::websocket::is_upgrade(req))
		{
#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
			zone->Text("WebSocket Upgrade");
#endif
			LogTrace(Channel::Web, "WebSocket upgrade requested");

			// Gate the upgrade through the same security policy as HTTP requests
			// (bearer token + Origin allow-list). Disabled by default => no change.
			if (auto rejection = Routing::AuthorizeWebSocketUpgrade(req, m_PeerIp); rejection.has_value())
			{
				DoWrite(std::move(*rejection));
				return;
			}

			// Capture the connection's revalidator NOW (valid only right after a
			// successful AuthorizeWebSocketUpgrade): Poll() re-checks it so a
			// revoked/expired credential closes the live socket (D15/D2). Empty
			// when auth-mode is off — then Poll performs no revalidation.
			m_WsRevalidator = Routing::CurrentWebSocketRevalidator();

			m_WsHandler = Routing::WS_OnAccept(req.target());
			if (!m_WsHandler)
			{
				LogDebug(Channel::Web, "No WebSocket handler found for target");
				DoWrite(HTTP::Responses::Response_404(req));
				return;
			}

			DoWsAccept(req);
			return;
		}

#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
		zone->Text(std::string(req.target().data(), req.target().size()));
#endif

		// Completion-based dispatch: every stock route completes inline (same
		// behaviour as the old synchronous call), while deferred-response
		// routes (login's off-thread argon2 verify) invoke the completion
		// later on this same io_context.  The session is kept alive by the
		// captured shared_ptr; DoWrite ignores completions after MarkDone.
		Routing::HTTP_OnRequestDispatch(std::move(req), m_PeerIp,
			[self = shared_from_this()](Message&& msg)
			{
				self->DoWrite(std::move(msg));
			});
	}

	//--- HTTP Write -------------------------------------------------------

	void HttpSessionState::DoWrite(Message msg)
	{
		if (m_Done) return;

		const bool keep_alive = msg.keep_alive();

		ArmTimeout();

		// The active-stream visitor is invoked at most once; move the move-only
		// message_generator into it and on into beast's async_write (which takes
		// ownership for the duration of the write).
		const bool dispatched = WithHttpStream(
			[self = shared_from_this(), keep_alive, msg = std::move(msg)](auto& stream) mutable
			{
				boost::beast::async_write(stream, std::move(msg),
					[self, keep_alive](boost::system::error_code ec, std::size_t bytes)
					{
						self->OnWrite(ec, bytes, keep_alive);
					});
			});

		if (!dispatched)
		{
			MarkDone();
		}
	}

	void HttpSessionState::OnWrite(boost::system::error_code ec, std::size_t bytes_transferred, bool keep_alive)
	{
		if (m_Done) return;

		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				LogDebug(Channel::Web, [&] { return std::format("HTTP write failed: {}", ec.message()); });
			}
			MarkDone();
			return;
		}

		LogTrace(Channel::Web, [&] { return std::format("HTTP response written ({} bytes)", bytes_transferred); });

		if (!keep_alive)
		{
			DoClose();
		}
		else
		{
			DoRead();
		}
	}

	//--- WebSocket Accept -------------------------------------------------

	void HttpSessionState::DoWsAccept(const Request& req)
	{
		if (m_Done) return;

		// Promote the active HTTP-phase stream into the matching WebSocket stream.
		// (This step is intrinsically per-type because the source/target stream
		// types differ; the option-setting + accept below is then shared.)
		if (m_SslStream)
		{
			boost::beast::get_lowest_layer(*m_SslStream).expires_never();
			m_WsSslStream.emplace(std::move(*m_SslStream));
			m_SslStream.reset();
		}
		else if (m_TcpStream)
		{
			m_TcpStream->expires_never();
			m_WsStream.emplace(std::move(*m_TcpStream));
			m_TcpStream.reset();
		}

		// The browser offers the `aqualink` subprotocol (carrying the bearer token
		// as a `bearer.<token>` entry when auth is enabled).  If — and only if — it
		// offered `aqualink`, the handshake response must echo it back: a browser
		// that offered subprotocols but received a response selecting none closes
		// the connection.  Auth itself was already enforced by
		// Routing::AuthorizeWebSocketUpgrade above.
		bool offered_aqualink = false;
		if (auto it = req.find(boost::beast::http::field::sec_websocket_protocol); it != req.end())
		{
			const std::string_view protocols{ it->value().data(), it->value().size() };
			std::size_t pos = 0;
			while (pos <= protocols.size())
			{
				const std::size_t comma = protocols.find(',', pos);
				std::string_view entry = (comma == std::string_view::npos) ? protocols.substr(pos) : protocols.substr(pos, comma - pos);
				while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) { entry.remove_prefix(1); }
				while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) { entry.remove_suffix(1); }
				if (entry == "aqualink") { offered_aqualink = true; break; }
				if (comma == std::string_view::npos) { break; }
				pos = comma + 1;
			}
		}

		const bool dispatched = WithWsStream([self = shared_from_this(), &req, offered_aqualink](auto& ws)
			{
				ws.set_option(
					boost::beast::websocket::stream_base::timeout::suggested(
						boost::beast::role_type::server));

				// Echo the negotiated subprotocol when the client offered `aqualink`.
				ws.set_option(boost::beast::websocket::stream_base::decorator(
					[offered_aqualink](boost::beast::websocket::response_type& res)
					{
						if (offered_aqualink)
						{
							res.set(boost::beast::http::field::sec_websocket_protocol, "aqualink");
						}
					}));

				// Cap inbound message size so a single frame cannot allocate beast's
				// 16 MB default; our JSON command messages are far smaller.
				ws.read_message_max(WS_READ_MESSAGE_MAX);

				ws.async_accept(req,
					[self](boost::system::error_code ec)
					{
						self->OnWsAccept(ec);
					});
			});

		if (!dispatched)
		{
			MarkDone();
		}
	}

	void HttpSessionState::OnWsAccept(boost::system::error_code ec)
	{
		if (m_Done) return;

		if (ec)
		{
			LogWarning(Channel::Web, [&] { return std::format("WebSocket accept failed: {}", ec.message()); });
			MarkDone();
			return;
		}

#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
		Factory::ProfilerFactory::Instance().Get()->Message("WebSocket: Upgrade accepted", static_cast<uint32_t>(Profiling::UnitColours::Cyan));
#endif
		m_WsConnectionId = m_WsHandler->OnOpen();
		m_WsActive = true;

		DoWsRead();
	}

	//--- WebSocket Read ---------------------------------------------------

	void HttpSessionState::DoWsRead()
	{
		if (m_Done) return;

		m_WsReadBuffer.clear();

		const bool dispatched = WithWsStream([self = shared_from_this(), this](auto& ws)
			{
				ws.async_read(m_WsReadBuffer,
					[self](boost::system::error_code ec, std::size_t bytes)
					{
						self->OnWsRead(ec, bytes);
					});
			});

		if (!dispatched)
		{
			MarkDone();
		}
	}

	void HttpSessionState::OnWsRead(boost::system::error_code ec, std::size_t bytes)
	{
		if (m_Done) return;

		if (ec == boost::beast::websocket::error::closed)
		{
			if (m_WsHandler) m_WsHandler->OnClose(m_WsConnectionId);
			MarkDone();
			return;
		}

		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				LogTrace(Channel::Web, [&] { return std::format("WebSocket read error: {}", ec.message()); });
				if (m_WsHandler) m_WsHandler->OnError(m_WsConnectionId);
			}
			MarkDone();
			return;
		}

		if (bytes > 0 && m_WsHandler)
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("HttpServer::ws_message", std::source_location::current());
			m_WsHandler->OnMessage(m_WsConnectionId, m_WsReadBuffer);
		}

		DoWsRead();
	}

	//--- WebSocket Write --------------------------------------------------

	void HttpSessionState::TryWsWrite()
	{
		if (m_Done || m_WsWriting || !m_WsHandler) return;

		auto msg = m_WsHandler->DequeueMessage(m_WsConnectionId);
		if (!msg) return;

		m_WsWriting = true;
		m_WsWriteBuffer = std::move(*msg);

		const bool dispatched = WithWsStream([self = shared_from_this(), this](auto& ws)
			{
				ws.binary(false);
				ws.async_write(boost::asio::buffer(m_WsWriteBuffer),
					[self](boost::system::error_code ec, std::size_t bytes)
					{
						self->OnWsWrite(ec, bytes);
					});
			});

		if (!dispatched)
		{
			m_WsWriting = false;
			MarkDone();
		}
	}

	void HttpSessionState::OnWsWrite(boost::system::error_code ec, std::size_t /*bytes*/)
	{
		m_WsWriting = false;

		if (m_Done) return;

		if (ec == boost::beast::websocket::error::closed)
		{
			if (m_WsHandler) m_WsHandler->OnClose(m_WsConnectionId);
			MarkDone();
			return;
		}

		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				LogDebug(Channel::Web, [&] { return std::format("WebSocket write error: {}", ec.message()); });
				if (m_WsHandler) m_WsHandler->OnError(m_WsConnectionId);
			}
			MarkDone();
			return;
		}

		if (m_WsHandler) m_WsHandler->OnPublish(m_WsConnectionId);

		// Chain: try writing the next queued message immediately
		TryWsWrite();
	}

	//--- Close / Cleanup --------------------------------------------------

	void HttpSessionState::DoClose()
	{
		if (m_Done) return;

		if (m_SslStream)
		{
			boost::beast::get_lowest_layer(*m_SslStream).expires_after(SESSION_TIMEOUT);

			m_SslStream->async_shutdown(
				[self = shared_from_this()](boost::system::error_code /*ec*/)
				{
					self->MarkDone();
				});
			return;
		}

		if (m_TcpStream)
		{
			boost::system::error_code ec;
			boost::beast::get_lowest_layer(*m_TcpStream).socket().shutdown(
				boost::asio::ip::tcp::socket::shutdown_send, ec);
		}

		MarkDone();
	}

	void HttpSessionState::MarkDone()
	{
		if (m_Done) return;

		m_Done = true;
		m_WsActive = false;
		m_WsHandler = nullptr;
	}

	//=========================================================================
	// HttpServer
	//=========================================================================

	HttpServer::HttpServer(boost::asio::io_context& io_context,
						   boost::asio::ip::tcp::endpoint endpoint,
						   std::optional<std::reference_wrapper<boost::asio::ssl::context>> ssl_context,
						   Routing::SecurityConfig security_config)
		: m_IoContext(io_context)
		, m_Endpoint(std::move(endpoint))
		, m_SslContext(ssl_context)
		, m_SecurityConfig(std::move(security_config))
	{
	}

	HttpServer::~HttpServer()
	{
		Stop();
	}

	bool HttpServer::Start()
	{
		boost::system::error_code ec;

		// Install the (opt-in) security policy into the shared Routing module so every
		// session enforces it. A default-constructed config is disabled (no change).
		Routing::SetSecurityConfig(m_SecurityConfig);

		m_Acceptor.emplace(m_IoContext);

		if (m_Acceptor->open(m_Endpoint.protocol(), ec); ec)
		{
			LogWarning(Channel::Web, [&] { return std::format("Failed to open HTTP acceptor: {}", ec.message()); });
			return false;
		}

		if (m_Acceptor->set_option(boost::asio::socket_base::reuse_address(true), ec); ec)
		{
			LogWarning(Channel::Web, [&] { return std::format("Failed to set reuse_address: {}", ec.message()); });
			return false;
		}

		if (m_Acceptor->bind(m_Endpoint, ec); ec)
		{
			LogWarning(Channel::Web, [&] { return std::format("Failed to bind to endpoint: {}", ec.message()); });
			return false;
		}

		if (m_Acceptor->listen(boost::asio::socket_base::max_listen_connections, ec); ec)
		{
			LogWarning(Channel::Web, [&] { return std::format("Failed to listen: {}", ec.message()); });
			return false;
		}

		m_Running = true;
		LogInfo(Channel::Web, [&] { return std::format("HTTP server listening on {}:{}", m_Endpoint.address().to_string(), m_Endpoint.port()); });

		DoAccept();

		return true;
	}

	void HttpServer::DoAccept()
	{
		if (!m_Running || !m_Acceptor) return;

		m_Acceptor->async_accept(
			[this](boost::system::error_code ec, TcpSocket socket)
			{
				OnAccept(ec, std::move(socket));
			});
	}

	void HttpServer::OnAccept(boost::system::error_code ec, TcpSocket socket)
	{
		if (!m_Running) return;

		if (ec)
		{
			if (ec != boost::asio::error::operation_aborted)
			{
				// A listener accept failure is actionable connectivity trouble; surface
				// it at the default log level rather than hiding it at Debug.
				LogWarning(Channel::Web, [&] { return std::format("Accept error: {}", ec.message()); });
			}

			if (m_Running)
			{
				DoAccept();
			}
			return;
		}

		// Security: Enforce connection limit to prevent resource exhaustion
		// Clean up completed sessions first
		std::erase_if(m_Sessions, [](const auto& s) { return !s || s->IsDone(); });

		if (m_Sessions.size() >= MAX_CONCURRENT_CONNECTIONS)
		{
			LogWarning(Channel::Web, [&] { return std::format("Connection limit ({}) reached, rejecting new connection", MAX_CONCURRENT_CONNECTIONS); });
			boost::system::error_code close_ec;
			socket.close(close_ec);
			DoAccept();
			return;
		}

		// Resolve the peer IP (empty if unavailable) for the per-IP connection cap
		// and the routing layer's per-source failed-auth rate limiter.
		std::string peer_ip;
		{
			boost::system::error_code rep_ec;
			const auto remote = socket.remote_endpoint(rep_ec);
			if (!rep_ec) { peer_ip = remote.address().to_string(); }
		}

		// Per-IP cap: a single client must not be able to consume the whole global
		// connection budget. Count live sessions already sharing this peer's IP.
		if (!peer_ip.empty())
		{
			const std::size_t from_this_ip = static_cast<std::size_t>(std::count_if(
				m_Sessions.begin(), m_Sessions.end(),
				[&peer_ip](const auto& s) { return s && s->PeerIp() == peer_ip; }));

			if (from_this_ip >= MAX_CONNECTIONS_PER_IP)
			{
				LogWarning(Channel::Web, [&] { return std::format("Per-IP connection limit ({}) reached for '{}', rejecting new connection", MAX_CONNECTIONS_PER_IP, peer_ip); });
				boost::system::error_code close_ec;
				socket.close(close_ec);
				DoAccept();
				return;
			}
		}

		LogInfo(Channel::Web, "Accepted new HTTP connection");
#if defined(TRACY_ENABLE) || defined(VTUNE_SUPPORT_ENABLED) || defined(UProf_SUPPORT_ENABLED)
		Factory::ProfilerFactory::Instance().Get()->Message("New HTTP connection accepted");
#endif

		std::shared_ptr<HttpSessionState> session;

		if (m_SslContext)
		{
			session = std::make_shared<HttpSessionState>(std::move(socket), m_SslContext->get(), std::move(peer_ip));
		}
		else
		{
			session = std::make_shared<HttpSessionState>(std::move(socket), std::move(peer_ip));
		}

		m_Sessions.push_back(session);
		session->Start();

		DoAccept();
	}

	void HttpServer::Poll()
	{
		if (!m_Running) return;

		// Nothing to drive when idle: skip the per-frame zone, PlotValue and sweep.
		if (m_Sessions.empty()) return;

		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("HttpServer::Poll", std::source_location::current());
		Factory::ProfilerFactory::Instance().Get()->PlotValue("Active HTTP Sessions", static_cast<int64_t>(m_Sessions.size()));

		// Kick WebSocket outbound writes. Track whether any session finished so the
		// (potentially reallocating) erase_if only runs when there is something to
		// remove, rather than scanning every session every frame.
		bool any_done = false;
		for (auto& session : m_Sessions)
		{
			if (session && !session->IsDone())
			{
				session->Poll();
			}

			if (!session || session->IsDone())
			{
				any_done = true;
			}
		}

		// Remove completed sessions only when at least one is actually done.
		if (any_done)
		{
			std::erase_if(m_Sessions, [](const auto& s) { return !s || s->IsDone(); });
		}
	}

	void HttpServer::Stop() noexcept
	{
		m_Running = false;

		if (m_Acceptor && m_Acceptor->is_open())
		{
			boost::system::error_code ec;
			m_Acceptor->close(ec);
		}

		for (auto& session : m_Sessions)
		{
			if (session) session->Close();
		}

		m_Sessions.clear();

		LogInfo(Channel::Web, "HTTP server stopped");
	}

}
// namespace AqualinkAutomate::HTTP
