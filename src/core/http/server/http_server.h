#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "interfaces/iwebsocket.h"

namespace AqualinkAutomate::HTTP
{

	/// Async HTTP session state machine.
	///
	/// Uses Beast async operations (async_read, async_write, etc.) that post
	/// completions to the io_context.  The main frame loop drives progress by
	/// calling io_context.poll().
	class HttpSessionState : public std::enable_shared_from_this<HttpSessionState>
	{
	public:
		explicit HttpSessionState(TcpSocket socket, std::string peer_ip = {});
		explicit HttpSessionState(TcpSocket socket, boost::asio::ssl::context& ssl_ctx, std::string peer_ip = {});

		void Start();
		void Poll();   // Kick WebSocket outbound writes
		bool IsDone() const;
		void Close();  // Force-close for shutdown

		// The connecting client's IP (empty when it could not be resolved). Used by
		// the server to enforce the per-IP connection cap and threaded into the
		// routing layer's per-source failed-auth rate limiter.
		const std::string& PeerIp() const { return m_PeerIp; }

	private:
		void DoSslHandshake();
		void OnSslHandshake(boost::system::error_code ec);

		void DoRead();
		void OnRead(boost::system::error_code ec, std::size_t bytes_transferred);

		void ProcessRequest();

		void DoWrite(Message msg);
		void OnWrite(boost::system::error_code ec, std::size_t bytes_transferred, bool keep_alive);

		void DoWsAccept(const Request& req);
		void OnWsAccept(boost::system::error_code ec);

		void DoWsRead();
		void OnWsRead(boost::system::error_code ec, std::size_t bytes_transferred);

		void TryWsWrite();
		void OnWsWrite(boost::system::error_code ec, std::size_t bytes_transferred);

		void DoClose();
		void MarkDone();

		// Re-arm the per-operation idle timeout on whichever HTTP-phase stream
		// (TLS or plain TCP) is currently active. Centralises the SSL-vs-TCP
		// branch that previously appeared at every async stage.
		void ArmTimeout();

		// Invoke fn with the active HTTP-phase stream (SslStream or TcpStream).
		// Returns false when neither is present (the session is being torn down).
		template<typename FN>
		bool WithHttpStream(FN&& fn)
		{
			if (m_SslStream) { fn(*m_SslStream); return true; }
			if (m_TcpStream) { fn(*m_TcpStream); return true; }
			return false;
		}

		// Invoke fn with the active WebSocket stream (WsSslStream or WsStream).
		// Returns false when neither is present.
		template<typename FN>
		bool WithWsStream(FN&& fn)
		{
			if (m_WsSslStream) { fn(*m_WsSslStream); return true; }
			if (m_WsStream) { fn(*m_WsStream); return true; }
			return false;
		}

	private:
		bool m_Done{ false };
		bool m_WsActive{ false };
		bool m_WsWriting{ false };

		bool m_HasSslContext{ false };
		boost::asio::ssl::context* m_SslContext{ nullptr };

		std::string m_PeerIp;

		std::optional<TcpStream> m_TcpStream;
		std::optional<SslStream> m_SslStream;

		boost::beast::flat_buffer m_Buffer;
		std::optional<boost::beast::http::request_parser<boost::beast::http::string_body>> m_Parser;

		std::optional<WsStream> m_WsStream;
		std::optional<WsSslStream> m_WsSslStream;
		Interfaces::IWebSocketBase* m_WsHandler{ nullptr };
		Interfaces::IWebSocketBase::ConnectionId m_WsConnectionId{ 0 };
		boost::beast::flat_buffer m_WsReadBuffer;
		std::string m_WsWriteBuffer;

		// Re-checks whether this WebSocket connection's credential is still
		// authorised (empty => auth-mode off, no revalidation); polled every
		// WS_REVALIDATE_INTERVAL frames. See Poll() and Routing::CurrentWebSocketRevalidator.
		Routing::WebSocketRevalidator m_WsRevalidator{};
		unsigned m_WsRevalidateTick{ 0 };

		// The poll loop runs at frame cadence (~10ms); re-validating every frame
		// would re-verify a JWT hundreds of times a second for no benefit, since
		// revocation/expiry move on the order of seconds. Check ~every 5s.
		static constexpr unsigned WS_REVALIDATE_INTERVAL = 500;

		static constexpr auto SESSION_TIMEOUT = std::chrono::seconds(30);

		// Cap an inbound WebSocket message. HTTP request bodies are already limited
		// to 10 KB (body_limit), but the WebSocket read previously relied solely on
		// Beast's 16 MB default, allowing a single frame to allocate far more than
		// the control-plane protocol ever needs. 64 KB is generous for our JSON
		// command messages while bounding per-connection memory.
		static constexpr std::uint64_t WS_READ_MESSAGE_MAX = 64U * 1024U;
	};

	/// Async HTTP server.
	///
	/// Accepts connections via async_accept and creates HttpSessionState
	/// instances.  The main frame loop must call io_context.poll() to drive
	/// all async completions, and Poll() to kick WebSocket outbound writes.
	class HttpServer
	{
	public:
		HttpServer(boost::asio::io_context& io_context,
				   boost::asio::ip::tcp::endpoint endpoint,
				   std::optional<std::reference_wrapper<boost::asio::ssl::context>> ssl_context = std::nullopt,
				   Routing::SecurityConfig security_config = {});
		~HttpServer();

		HttpServer(const HttpServer&) = delete;
		HttpServer& operator=(const HttpServer&) = delete;

		bool Start();
		void Poll();
		void Stop() noexcept;

	private:
		void DoAccept();
		void OnAccept(boost::system::error_code ec, TcpSocket socket);

		boost::asio::io_context& m_IoContext;
		boost::asio::ip::tcp::endpoint m_Endpoint;
		std::optional<TcpAcceptor> m_Acceptor;
		std::optional<std::reference_wrapper<boost::asio::ssl::context>> m_SslContext;

		// Opt-in security policy (bearer token / Origin allow-list / CSRF header).
		// Default-constructed = disabled = historical behaviour. Installed into the
		// shared Routing module on Start() so every session enforces it uniformly.
		Routing::SecurityConfig m_SecurityConfig;

		std::vector<std::shared_ptr<HttpSessionState>> m_Sessions;
		bool m_Running{ false };

		// Security: Maximum concurrent connections to prevent resource exhaustion
		static constexpr std::size_t MAX_CONCURRENT_CONNECTIONS = 1000;

		// Per-source-IP connection cap so a single client cannot consume the whole
		// global budget (DoS). Generous for a browser (HTTP + WebSocket + a few
		// parallel fetches) while bounding any one peer.
		static constexpr std::size_t MAX_CONNECTIONS_PER_IP = 50;
	};

}
// namespace AqualinkAutomate::HTTP
