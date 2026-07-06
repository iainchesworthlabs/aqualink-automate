#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/ssl.h>

#include <async_mqtt/all.hpp>
#include <async_mqtt/asio_bind/predefined_layer/mqtts.hpp>

#include "logging/logging.h"
#include "logging/log_sanitize.h"
#include "mqtt/mqtt_client.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace am = async_mqtt;
namespace as = boost::asio;

namespace AqualinkAutomate::Mqtt
{

	//=========================================================================
	// Impl — owns the async_mqtt endpoint, the outbound publish queue, and the
	// reconnect timer.  A nested type (defined here, in the .cpp) so the heavy
	// async_mqtt template instantiations happen exactly once in this TU.
	//=========================================================================
	struct MqttClient::Impl
	{
		using TcpEndpoint = am::endpoint<am::role::client, am::protocol::mqtt>;
		using TlsEndpoint = am::endpoint<am::role::client, am::protocol::mqtts>;

		Impl(MqttClient& owner, as::io_context& io_context)
			: m_Owner(owner)
			, m_IoContext(io_context)
			, m_ReconnectTimer(io_context)
		{
		}

		MqttClient& m_Owner;
		as::io_context& m_IoContext;
		as::steady_timer m_ReconnectTimer;

		bool m_Tls{ false };
		am::protocol_version m_Version{ am::protocol_version::v3_1_1 };

		std::optional<as::ssl::context> m_SslContext;
		std::shared_ptr<TcpEndpoint> m_TcpEp;
		std::shared_ptr<TlsEndpoint> m_TlsEp;

		//---------------------------------------------------------------------
		// Endpoint dispatch helpers
		//---------------------------------------------------------------------

		// True iff `ep` is still the active endpoint (a Stop()/reconnect resets the
		// member, so a late completion handler from a superseded connection sees
		// this as false and bows out without mutating state).
		template <class Ep>
		bool IsCurrent(const std::shared_ptr<Ep>& ep) const noexcept
		{
			if (!m_Owner.m_Running)
			{
				return false;
			}
			if constexpr (std::is_same_v<Ep, TlsEndpoint>)
			{
				return m_TlsEp == ep;
			}
			else
			{
				return m_TcpEp == ep;
			}
		}

		template <class F>
		void WithEndpoint(F&& f)
		{
			if (m_Tls)
			{
				if (m_TlsEp) { f(m_TlsEp); }
			}
			else
			{
				if (m_TcpEp) { f(m_TcpEp); }
			}
		}

		//---------------------------------------------------------------------
		// Connection lifecycle
		//---------------------------------------------------------------------

		void BeginConnect()
		{
			m_Owner.m_State = State::Connecting;

			// The async connect chain captures shared_from_this() to stay alive
			// across pending handlers, so the client must be owned by a shared_ptr
			// (it always is in production via make_shared). A stack-constructed
			// client (as in some unit tests) stays in Connecting with no live
			// endpoint rather than throwing bad_weak_ptr.
			if (m_Owner.weak_from_this().expired())
			{
				LogError(Channel::Mqtt, "MqttClient must be shared_ptr-owned to connect; no endpoint started");
				return;
			}

			Factory::ProfilerFactory::Instance().Get()->Message("MQTT: Connecting to broker", static_cast<uint32_t>(UnitColours::Cyan));

			if (m_Tls)
			{
				BuildSslContext();
				m_TlsEp = std::make_shared<TlsEndpoint>(m_Version, m_IoContext.get_executor(), *m_SslContext);

				// SNI + RFC 6125 hostname verification on the TLS next-layer
				// (mqtts == ssl::stream<tcp>). Peer + trust-store verification is
				// configured on the context in BuildSslContext().
				auto& ssl_stream = m_TlsEp->next_layer();
				if (!SSL_set_tlsext_host_name(ssl_stream.native_handle(), m_Owner.m_Settings.broker_host.c_str()))
				{
					LogWarning(Channel::Mqtt, "Failed to set SNI hostname");
				}
				if (!m_Owner.m_Settings.tls.tls_skip_verify)
				{
					boost::system::error_code verify_ec;
					ssl_stream.set_verify_callback(as::ssl::host_name_verification(m_Owner.m_Settings.broker_host), verify_ec);
					if (verify_ec)
					{
						LogError(Channel::Mqtt, [msg = verify_ec.message()] { return std::format("Failed to install hostname verification: {}", msg); });
					}
				}

				m_TlsEp->set_pingreq_send_interval(std::chrono::seconds(KEEPALIVE_SECONDS));
				DoHandshake(m_TlsEp);
			}
			else
			{
				m_TcpEp = std::make_shared<TcpEndpoint>(m_Version, m_IoContext.get_executor());
				m_TcpEp->set_pingreq_send_interval(std::chrono::seconds(KEEPALIVE_SECONDS));
				DoHandshake(m_TcpEp);
			}
		}

		template <class Ep>
		void DoHandshake(std::shared_ptr<Ep> ep)
		{
			auto self = m_Owner.shared_from_this();
			ep->async_underlying_handshake(
				m_Owner.m_Settings.broker_host,
				std::to_string(m_Owner.m_Settings.broker_port),
				[this, self, ep](am::error_code ec)
				{
					if (!IsCurrent(ep)) { return; }
					if (ec)
					{
						m_Owner.OnError(std::format("Underlying handshake failed: {}", ec.message()));
						ScheduleReconnect(std::format("Handshake failed to {}:{}: {}",
							m_Owner.m_Settings.broker_host, m_Owner.m_Settings.broker_port, ec.message()));
						return;
					}
					DoSendConnect(ep);
				});
		}

		template <class Ep>
		void DoSendConnect(std::shared_ptr<Ep> ep)
		{
			// Will (optional). MQTT 3.1.1 has no will properties; v5 ignores the
			// empty property set, so a single construction serves both versions.
			std::optional<am::will> will;
			if (m_Owner.m_WillConfig)
			{
				am::pub::opts will_opts = am::qos::at_most_once;
				if (m_Owner.m_WillConfig->retain) { will_opts = will_opts | am::pub::retain::yes; }
				will.emplace(m_Owner.m_WillConfig->topic, m_Owner.m_WillConfig->payload, will_opts);
			}

			// MQTT [MQTT-3.1.2-22]: the password is only valid alongside a username.
			std::optional<std::string> user;
			std::optional<std::string> pass;
			if (!m_Owner.m_Settings.username.empty())
			{
				user = m_Owner.m_Settings.username;
				if (!m_Owner.m_Settings.password.empty()) { pass = m_Owner.m_Settings.password; }
			}
			else if (!m_Owner.m_Settings.password.empty())
			{
				LogWarning(Channel::Mqtt, "MQTT password configured without a username - omitting password from CONNECT");
			}

			auto self = m_Owner.shared_from_this();
			auto cb = [this, self, ep](am::error_code ec)
			{
				if (!IsCurrent(ep)) { return; }
				if (ec)
				{
					ScheduleReconnect(std::format("CONNECT send failed: {}", ec.message()));
					return;
				}
				DoRecvConnack(ep);
			};

			if (m_Version == am::protocol_version::v5)
			{
				ep->async_send(am::v5::connect_packet{ true, KEEPALIVE_SECONDS, m_Owner.m_ClientId, will, user, pass }, std::move(cb));
			}
			else
			{
				ep->async_send(am::v3_1_1::connect_packet{ true, KEEPALIVE_SECONDS, m_Owner.m_ClientId, will, user, pass }, std::move(cb));
			}
		}

		template <class Ep>
		void DoRecvConnack(std::shared_ptr<Ep> ep)
		{
			auto self = m_Owner.shared_from_this();
			ep->async_recv(
				[this, self, ep](am::error_code ec, std::optional<am::packet_variant> pv)
				{
					HandleConnack(ep, ec, std::move(pv));
				});
		}

		template <class Ep>
		void HandleConnack(const std::shared_ptr<Ep>& ep, am::error_code ec, std::optional<am::packet_variant> pv)
		{
			if (!IsCurrent(ep)) { return; }
			if (ec)
			{
				ScheduleReconnect(std::format("CONNACK recv error: {}", ec.message()));
				return;
			}

			bool accepted = false;
			if (pv)
			{
				pv->visit(am::overload{
					[&accepted](am::v3_1_1::connack_packet const& p) { accepted = (p.code() == am::connect_return_code::accepted); },
					[&accepted](am::v5::connack_packet const& p) { accepted = (p.code() == am::connect_reason_code::success); },
					[](auto const&) { /* Non-CONNACK packets are not expected here and are ignored. */ }
				});
			}

			if (!accepted)
			{
				ScheduleReconnect("CONNACK rejected by broker");
				return;
			}

			m_Owner.m_State = State::Connected;
			m_Owner.m_ReconnectAttempts = 0;
			m_Owner.m_LastError.clear();

			LogInfo(Channel::Mqtt, [this] { return std::format("Connected to MQTT broker at {}:{}{} (MQTT {})",
				m_Owner.m_Settings.broker_host, m_Owner.m_Settings.broker_port,
				m_Owner.m_Settings.tls.use_tls ? " (TLS)" : "",
				Options::Mqtt::ToString(m_Owner.m_Settings.protocol_version)); });
			Factory::ProfilerFactory::Instance().Get()->Message("MQTT: Connected", static_cast<uint32_t>(UnitColours::Green));

			m_Owner.OnConnected();

			DoRecvLoop(ep);
			DoFlush(ep);
		}

		template <class Ep>
		void DoRecvLoop(std::shared_ptr<Ep> ep)
		{
			auto self = m_Owner.shared_from_this();
			ep->async_recv(
				[this, self, ep](am::error_code ec, std::optional<am::packet_variant> pv)
				{
					if (!IsCurrent(ep)) { return; }
					if (ec)
					{
						Factory::ProfilerFactory::Instance().Get()->Message("MQTT: Disconnected", static_cast<uint32_t>(UnitColours::Orange));
						ScheduleReconnect(std::format("Connection lost: {}", ec.message()), /*emit_disconnect=*/true);
						return;
					}

					if (pv)
					{
						pv->visit(am::overload{
							[this](am::v3_1_1::publish_packet const& p) { DeliverPublish(p.topic(), p.payload()); },
							[this](am::v5::publish_packet const& p) { DeliverPublish(p.topic(), p.payload()); },
							[](auto const&) { /* Only PUBLISH packets carry application messages; others are ignored. */ }
						});
					}

					DoRecvLoop(ep);
				});
		}

		void DeliverPublish(const std::string& topic, const std::string& payload)
		{
			LogDebug(Channel::Mqtt, [&topic, &payload] { return std::format("Received PUBLISH: topic='{}', payload_size={}", Logging::SanitizeForLog(topic), payload.size()); });
			m_Owner.OnMessageReceived(topic, payload);
		}

		//---------------------------------------------------------------------
		// Publish / subscribe
		//---------------------------------------------------------------------

		void Enqueue(const std::string& topic, const std::string& payload, bool retain)
		{
			if (m_Owner.m_PublishQueue.size() >= MAX_PUBLISH_QUEUE_SIZE)
			{
				LogWarning(Channel::Mqtt, "MQTT publish queue full, dropping oldest message");
				m_Owner.m_PublishQueue.pop_front();
				++m_Owner.m_DroppedCount;
			}
			m_Owner.m_PublishQueue.emplace_back(topic, payload, retain);
		}

		void FlushIfConnected()
		{
			if (m_Owner.m_State != State::Connected)
			{
				return;
			}
			Factory::ProfilerFactory::Instance().Get()->PlotValue("MQTT Publish Queue", static_cast<int64_t>(m_Owner.m_PublishQueue.size()));
			WithEndpoint([this](auto& ep) { DoFlush(ep); });
		}

		template <class Ep>
		void DoFlush(std::shared_ptr<Ep> ep)
		{
			while (!m_Owner.m_PublishQueue.empty())
			{
				auto item = std::move(m_Owner.m_PublishQueue.front());
				m_Owner.m_PublishQueue.pop_front();

				am::pub::opts opts = am::qos::at_most_once;
				if (item.retain) { opts = opts | am::pub::retain::yes; }

				auto self = m_Owner.shared_from_this();
				auto cb = [this, self, ep](am::error_code ec)
				{
					if (!IsCurrent(ep)) { return; }
					if (ec)
					{
						LogWarning(Channel::Mqtt, [msg = ec.message()] { return std::format("PUBLISH send error: {}", msg); });
						// The recv loop observes the same disconnect and drives reconnect.
					}
					else
					{
						++m_Owner.m_PublishedCount;
					}
				};

				if (m_Version == am::protocol_version::v5)
				{
					ep->async_send(am::v5::publish_packet{ item.topic, item.payload, opts }, std::move(cb));
				}
				else
				{
					ep->async_send(am::v3_1_1::publish_packet{ item.topic, item.payload, opts }, std::move(cb));
				}
			}
		}

		void Subscribe(const std::string& filter, std::uint8_t /*qos*/)
		{
			WithEndpoint([this, &filter](auto& ep) { DoSubscribe(ep, filter); });
		}

		template <class Ep>
		void DoSubscribe(std::shared_ptr<Ep>& ep, const std::string& filter)
		{
			auto pid = ep->acquire_unique_packet_id();
			if (!pid)
			{
				LogWarning(Channel::Mqtt, [&filter] { return std::format("Cannot subscribe to '{}': no packet id available", filter); });
				return;
			}

			auto self = m_Owner.shared_from_this();
			auto cb = [self, filter](am::error_code ec)
			{
				if (ec)
				{
					LogWarning(Channel::Mqtt, [&filter, msg = ec.message()] { return std::format("Failed to send SUBSCRIBE for '{}': {}", filter, msg); });
				}
			};

			// The endpoint API is identical for TCP/TLS (ep is deduced); only the
			// packet type differs by protocol version.
			if (m_Version == am::protocol_version::v5)
			{
				ep->async_send(am::v5::subscribe_packet{ *pid, { { filter, am::qos::at_most_once } } }, std::move(cb));
			}
			else
			{
				ep->async_send(am::v3_1_1::subscribe_packet{ *pid, { { filter, am::qos::at_most_once } } }, std::move(cb));
			}
			LogDebug(Channel::Mqtt, [&filter] { return std::format("Sent SUBSCRIBE for '{}'", filter); });
		}

		//---------------------------------------------------------------------
		// Teardown / reconnect
		//---------------------------------------------------------------------

		void ScheduleReconnect(const std::string& reason, bool emit_disconnect = false)
		{
			LogWarning(Channel::Mqtt, [this, &reason] { return std::format("MQTT reconnecting (attempt {}): {}", m_Owner.m_ReconnectAttempts + 1, reason); });

			m_Owner.m_LastError = reason;
			CloseActive();
			m_Owner.m_State = State::Reconnecting;
			++m_Owner.m_ReconnectAttempts;

			if (emit_disconnect)
			{
				m_Owner.OnDisconnected(reason);
			}

			if (!m_Owner.m_Running)
			{
				return;
			}

			auto self = m_Owner.shared_from_this();
			m_ReconnectTimer.expires_after(m_Owner.CalculateReconnectDelay());
			m_ReconnectTimer.async_wait(
				[this, self](am::error_code ec)
				{
					if (ec || !m_Owner.m_Running)
					{
						return;
					}
					LogInfo(Channel::Mqtt, [this] { return std::format("Attempting reconnection (attempt {})", m_Owner.m_ReconnectAttempts); });
					BeginConnect();
				});
		}

		// Tear the active endpoint down. Resetting the member first makes any
		// in-flight completion handler's IsCurrent() return false; consign keeps the
		// endpoint alive until its async_close completes so pending ops unwind
		// cleanly (async_mqtt does not hold the endpoint alive for us).
		void CloseActive() noexcept
		{
			if (m_TcpEp)
			{
				auto ep = m_TcpEp;
				m_TcpEp.reset();
				ep->async_close(as::consign(as::detached, ep));
			}
			if (m_TlsEp)
			{
				auto ep = m_TlsEp;
				m_TlsEp.reset();
				ep->async_close(as::consign(as::detached, ep));
			}
		}

		void Shutdown() noexcept
		{
			m_ReconnectTimer.cancel();
			CloseActive();
			m_Owner.m_PublishQueue.clear();
		}

		// Load the optional client certificate/key pair into the SSL context.
		// `ec` is threaded through so a failure surfaces to BuildSslContext exactly
		// as it did inline (the async_mqtt/asio API reports via error_code).
		void LoadClientCertificate(boost::system::error_code& ec)
		{
			if (m_Owner.m_Settings.tls.tls_client_cert.empty() || m_Owner.m_Settings.tls.tls_client_key.empty())
			{
				return;
			}

			if (!std::filesystem::exists(m_Owner.m_Settings.tls.tls_client_cert) || !std::filesystem::exists(m_Owner.m_Settings.tls.tls_client_key))
			{
				LogError(Channel::Mqtt, "Client certificate or key file not found");
				return;
			}

			m_SslContext->use_certificate_file(m_Owner.m_Settings.tls.tls_client_cert, as::ssl::context::pem, ec);
			if (ec)
			{
				LogError(Channel::Mqtt, [msg = ec.message()] { return std::format("Failed to load client certificate: {}", msg); });
				return;
			}

			m_SslContext->use_private_key_file(m_Owner.m_Settings.tls.tls_client_key, as::ssl::context::pem, ec);
			if (ec) { LogError(Channel::Mqtt, [msg = ec.message()] { return std::format("Failed to load client private key: {}", msg); }); }
		}

		void BuildSslContext()
		{
			LogInfo(Channel::Mqtt, "Initializing TLS context for MQTT");

			m_SslContext.emplace(as::ssl::context::tlsv12_client);
			m_SslContext->set_options(
				as::ssl::context::default_workarounds |
				as::ssl::context::no_sslv2 |
				as::ssl::context::no_sslv3 |
				as::ssl::context::no_tlsv1 |
				as::ssl::context::no_tlsv1_1);

			boost::system::error_code ec;

			if (!m_Owner.m_Settings.tls.tls_ca_cert.empty())
			{
				if (std::filesystem::exists(m_Owner.m_Settings.tls.tls_ca_cert))
				{
					m_SslContext->load_verify_file(m_Owner.m_Settings.tls.tls_ca_cert, ec);
					if (ec) { LogError(Channel::Mqtt, [msg = ec.message()] { return std::format("Failed to load CA certificate: {}", msg); }); }
				}
				else
				{
					LogError(Channel::Mqtt, [this] { return std::format("CA certificate file not found: {}", m_Owner.m_Settings.tls.tls_ca_cert); });
				}
			}

			LoadClientCertificate(ec);

			if (m_Owner.m_Settings.tls.tls_skip_verify)
			{
				LogWarning(Channel::Mqtt, "TLS certificate verification DISABLED - not recommended for production");
				m_SslContext->set_verify_mode(as::ssl::verify_none);
			}
			else
			{
				m_SslContext->set_verify_mode(as::ssl::verify_peer);
				m_SslContext->set_default_verify_paths(ec);
				if (ec) { LogWarning(Channel::Mqtt, [msg = ec.message()] { return std::format("Failed to load system trust store: {}", msg); }); }
			}
		}
	};

	//=========================================================================
	// MqttClient — thin, async_mqtt-free facade. All endpoint work delegates to
	// Impl; this object owns the observable state/diagnostics + the signals.
	//=========================================================================

	MqttClient::MqttClient(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings)
		: m_IoContext(io_context)
		, m_Settings(settings)
		, m_Impl(std::make_unique<Impl>(*this, io_context))
	{
		m_ClientId = settings.client_id.empty() ? GenerateClientId() : settings.client_id;
		LogDebug(Channel::Mqtt, [this, &settings] { return std::format("MQTT client created with client ID: {} (MQTT {})", m_ClientId, Options::Mqtt::ToString(settings.protocol_version)); });

		if (!m_Settings.password.empty() && !m_Settings.tls.use_tls)
		{
			LogWarning(Channel::Mqtt, "MQTT credentials configured without TLS - password will be sent in cleartext; enable --mqtt-tls");
		}
	}

	MqttClient::~MqttClient()
	{
		Stop();
	}

	void MqttClient::SetWill(const std::string& topic, const std::string& payload, bool retain)
	{
		m_WillConfig = WillConfig{ topic, payload, retain };
		LogDebug(Channel::Mqtt, [&topic, &retain] { return std::format("LWT configured: topic='{}', retain={}", topic, retain); });
	}

	void MqttClient::Start()
	{
		if (m_Running)
		{
			LogWarning(Channel::Mqtt, "MQTT client already running");
			return;
		}

		m_Running = true;
		m_ReconnectAttempts = 0;
		m_State = State::Connecting;

		m_Impl->m_Tls = m_Settings.tls.use_tls;
		m_Impl->m_Version = (m_Settings.protocol_version == Options::Mqtt::ProtocolVersion::v5)
			? am::protocol_version::v5
			: am::protocol_version::v3_1_1;

		LogInfo(Channel::Mqtt, [this] { return std::format("Starting MQTT client, connecting to {}:{} (MQTT {})",
			m_Settings.broker_host, m_Settings.broker_port, Options::Mqtt::ToString(m_Settings.protocol_version)); });

		m_Impl->BeginConnect();
	}

	void MqttClient::Stop() noexcept
	{
		if (!m_Running)
		{
			return;
		}

		LogInfo(Channel::Mqtt, "Stopping MQTT client");
		m_Running = false;

		m_Impl->Shutdown();
		m_State = State::Disconnected;

		OnDisconnected("Client stopped");
	}

	void MqttClient::Poll()
	{
		if (!m_Running)
		{
			return;
		}

		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("MqttClient::Poll", std::source_location::current());

		// Drain any queued publishes once connected. async_mqtt's completion
		// handlers (handshake/recv/keepalive/reconnect) advance via the host
		// io_context.poll(); nothing else is pumped here.
		m_Impl->FlushIfConnected();
	}

	bool MqttClient::IsConnected() const noexcept
	{
		return m_State == State::Connected;
	}

	bool MqttClient::IsRunning() const noexcept
	{
		return m_Running;
	}

	MqttClient::State MqttClient::GetState() const noexcept
	{
		return m_State;
	}

	const std::string& MqttClient::ClientId() const noexcept
	{
		return m_ClientId;
	}

	const std::optional<MqttClient::WillConfig>& MqttClient::GetWill() const noexcept
	{
		return m_WillConfig;
	}

	void MqttClient::Publish(const std::string& topic, const std::string& payload, bool retain)
	{
		m_Impl->Enqueue(topic, payload, retain);
	}

	void MqttClient::Subscribe(const std::string& topic_filter, uint8_t qos)
	{
		if (m_State != State::Connected)
		{
			LogWarning(Channel::Mqtt, [&topic_filter] { return std::format("Cannot subscribe to '{}': not connected", topic_filter); });
			return;
		}
		m_Impl->Subscribe(topic_filter, qos);
	}

	std::string MqttClient::BuildTopic(const std::string& subtopic) const
	{
		if (m_Settings.topic_prefix.empty())
		{
			return subtopic;
		}
		return std::format("{}/{}", m_Settings.topic_prefix, subtopic);
	}

	const std::string& MqttClient::TopicPrefix() const noexcept
	{
		return m_Settings.topic_prefix;
	}

	std::string MqttClient::GenerateClientId() const
	{
		std::random_device rd;
		std::uniform_int_distribution<> dis(0, 15);

		const char* hex = "0123456789abcdef";
		std::string result = "aqualink-";

		for (int i = 0; i < 8; ++i)
		{
			result += hex[dis(rd)];
		}

		return result;
	}

	std::chrono::seconds MqttClient::CalculateReconnectDelay() const
	{
		using Rep = std::chrono::seconds::rep;
		auto base_delay = m_Settings.reconnect_delay_initial.count();
		auto max_delay = m_Settings.reconnect_delay_max.count();

		auto delay = base_delay * (Rep{ 1 } << std::min(m_ReconnectAttempts, static_cast<uint16_t>(10)));
		delay = std::min(delay, max_delay);

		std::random_device rd;
		std::uniform_int_distribution<> jitter(0, static_cast<int>(delay / 4));
		delay += jitter(rd);

		return std::chrono::seconds(delay);
	}

}
// namespace AqualinkAutomate::Mqtt
