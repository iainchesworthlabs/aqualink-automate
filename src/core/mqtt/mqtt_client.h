#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/signals2.hpp>

#include "options/options_mqtt_options.h"

namespace AqualinkAutomate::Test { class MqttClientPacketTest; }

namespace AqualinkAutomate::Mqtt
{

	/// Asynchronous MQTT client backed by the async_mqtt library (header-only,
	/// Boost.Asio based). Speaks MQTT 3.1.1 or 5.0 — selected at runtime from
	/// settings.protocol_version — over plain TCP or TLS.
	///
	/// Thread-safety contract: This class is NOT thread-safe. All public methods
	/// (Publish(), Poll(), Start(), Stop(), Subscribe(), ...) must be called from
	/// the same thread that runs the associated io_context. The client is driven
	/// cooperatively: the async_mqtt completion handlers advance whenever
	/// io_context.poll()/run() executes ready handlers (the host frame loop already
	/// pumps the io_context). A slow or unreachable broker never blocks the loop —
	/// name resolution, connect, the TLS handshake and the CONNECT/CONNACK exchange
	/// are all asynchronous.
	///
	/// Reconnection (exponential backoff + jitter) and the bounded, drop-oldest
	/// outbound publish queue are managed here; async_mqtt itself does not
	/// auto-reconnect. Framing, partial I/O, keep-alive PINGREQ and packet
	/// encode/decode are owned by async_mqtt.
	///
	/// The async_mqtt types are confined to the .cpp behind a pimpl so neither the
	/// MQTT hub/integration/diagnostics consumers nor the unit tests pay the (heavy,
	/// template-instantiation) compile cost of the library.
	class MqttClient : public std::enable_shared_from_this<MqttClient>
	{
	public:
		using ConnectedSignal = boost::signals2::signal<void()>;
		using DisconnectedSignal = boost::signals2::signal<void(const std::string& reason)>;
		using MessageReceivedSignal = boost::signals2::signal<void(const std::string& topic, const std::string& payload)>;
		using ErrorSignal = boost::signals2::signal<void(const std::string& error_message)>;

		enum class State
		{
			Disconnected,
			Connecting,
			Connected,
			Reconnecting
		};

	public:
		MqttClient(boost::asio::io_context& io_context, const Options::Mqtt::MqttSettings& settings);
		~MqttClient();

		MqttClient(const MqttClient&) = delete;
		MqttClient& operator=(const MqttClient&) = delete;

		struct WillConfig
		{
			std::string topic;
			std::string payload;
			bool retain{ true };
		};

		void SetWill(const std::string& topic, const std::string& payload, bool retain = true);

		void Start();
		void Stop() noexcept;
		void Poll();

		bool IsConnected() const noexcept;
		bool IsRunning() const noexcept;

		State GetState() const noexcept;
		const std::string& ClientId() const noexcept;
		const std::optional<WillConfig>& GetWill() const noexcept;

		void Publish(const std::string& topic, const std::string& payload, bool retain = false);
		void Subscribe(const std::string& topic_filter, uint8_t qos = 0);
		std::string BuildTopic(const std::string& subtopic) const;
		const std::string& TopicPrefix() const noexcept;

		// Read-only diagnostics accessors. Safe to call from the HTTP handler: the
		// client is single-threaded and the handler runs on the same io_context
		// thread, so no synchronisation is required.
		const Options::Mqtt::MqttSettings& Settings() const noexcept { return m_Settings; }
		std::size_t PublishQueueDepth() const noexcept { return m_PublishQueue.size(); }
		std::uint16_t ReconnectAttempts() const noexcept { return m_ReconnectAttempts; }
		std::uint64_t PublishedCount() const noexcept { return m_PublishedCount; }
		std::uint64_t DroppedCount() const noexcept { return m_DroppedCount; }
		const std::string& LastError() const noexcept { return m_LastError; }

	public:
		ConnectedSignal OnConnected;
		DisconnectedSignal OnDisconnected;
		MessageReceivedSignal OnMessageReceived;
		ErrorSignal OnError;

	private:
		// async_mqtt-backed implementation: the (TCP or TLS) endpoint and the
		// reconnect timer. A nested type defined in the .cpp so the heavy async_mqtt
		// templates instantiate exactly once. It drains sends from m_PublishQueue.
		struct Impl;

		// A queued outbound publish. Kept in the facade (plain data, no async_mqtt)
		// so the publish / drop-oldest behaviour stays unit-testable without the
		// library, and inspectable by the test seam below.
		struct PendingPublish
		{
			std::string topic;
			std::string payload;
			bool retain{ false };
		};

		std::string GenerateClientId() const;
		std::chrono::seconds CalculateReconnectDelay() const;

		// Test seam: inspect the publish queue / force the connected state.
		friend class AqualinkAutomate::Test::MqttClientPacketTest;

	private:
		boost::asio::io_context& m_IoContext;
		const Options::Mqtt::MqttSettings m_Settings;

		std::string m_ClientId;
		std::optional<WillConfig> m_WillConfig;

		State m_State{ State::Disconnected };
		bool m_Running{ false };

		// Diagnostics counters (surfaced via /api/diagnostics/mqtt). Mutated by Impl.
		std::uint16_t m_ReconnectAttempts{ 0 };
		std::uint64_t m_PublishedCount{ 0 };
		std::uint64_t m_DroppedCount{ 0 };
		std::string m_LastError;

		// Outbound publish queue, drained by Impl once connected. Bounded with
		// drop-oldest semantics (see MAX_PUBLISH_QUEUE_SIZE).
		std::deque<PendingPublish> m_PublishQueue;

		// Security: bound the outbound queue (drop-oldest) to prevent memory
		// exhaustion if the broker is unreachable for a long time.
		static constexpr std::size_t MAX_PUBLISH_QUEUE_SIZE = 1000;

		// MQTT keep-alive (seconds) advertised in CONNECT. async_mqtt sends PINGREQ
		// automatically at this interval (set_pingreq_send_interval).
		static constexpr std::uint16_t KEEPALIVE_SECONDS = 60;

		// Owns the async_mqtt endpoint + queue + reconnect timer. Destroyed (in the
		// .cpp, where Impl is complete) by the out-of-line destructor.
		std::unique_ptr<Impl> m_Impl;
	};

}
// namespace AqualinkAutomate::Mqtt
