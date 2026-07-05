#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

namespace AqualinkAutomate::Test
{

//=============================================================================
// In-process MQTT 3.1.1 mock broker(s) over loopback, for driving the REAL
// MqttClient async connection flow (which a broker-less client never reaches).
//
// MqttWireCore<Derived> owns the MQTT 3.1.1 framing/handling and the
// observation counters; the two transports (plain TCP and TLS) supply the
// stream I/O via CRTP (Derived::SendRaw). Both run on the SAME cooperative
// io_context as the client — pump that context (run_for/poll) to advance the
// acceptor, the client's connect and the broker's reads together on one thread.
//
// It is NOT a conforming broker: it speaks exactly the packets the client
// sends and replies with the minimum the client needs. Fixed-header wire form:
// byte0 = type<<4 | flags, then a 1–4 byte varint "remaining length", then that
// many body bytes.
//=============================================================================
template <class Derived>
class MqttWireCore
{
public:
	// Observation accessors for assertions.
	bool ClientConnected() const { return m_SawConnect; }
	bool ClientDisconnected() const { return m_SawDisconnect; }
	std::size_t PublishesReceived() const { return m_PublishesReceived; }
	std::size_t SubscribesReceived() const { return m_SubscribesReceived; }
	const std::string& LastPublishTopic() const { return m_LastPublishTopic; }

	// When set, the broker answers CONNECT with a non-accepted CONNACK so the
	// client's "CONNACK rejected" -> reconnect branch runs.
	void SetRejectConnect(bool reject) { m_RejectConnect = reject; }

	// Push a QoS-0 PUBLISH to the connected client (drives its recv loop + the
	// MessageReceived signal).
	void PublishToClient(const std::string& topic, const std::string& payload)
	{
		std::string body;
		body.push_back(static_cast<char>((topic.size() >> 8) & 0xFF));
		body.push_back(static_cast<char>(topic.size() & 0xFF));
		body += topic;
		body += payload;   // QoS 0 => no packet identifier
		static_cast<Derived*>(this)->SendRaw(Frame(0x30, body));
	}

protected:
	// Append received bytes and dispatch every complete packet now buffered.
	void FeedBytes(const char* data, std::size_t n)
	{
		m_Rx.append(data, n);
		for (;;)
		{
			if (m_Rx.size() < 2) { return; }

			std::size_t multiplier = 1;
			std::size_t rem_len = 0;
			std::size_t header_len = 1;   // byte0
			bool complete_len = false;
			for (std::size_t i = 1; i < m_Rx.size() && i <= 4; ++i)
			{
				const auto byte = static_cast<std::uint8_t>(m_Rx[i]);
				rem_len += (byte & 0x7F) * multiplier;
				multiplier *= 128;
				++header_len;
				if (0 == (byte & 0x80)) { complete_len = true; break; }
			}
			if (!complete_len) { return; }                         // varint not fully buffered
			if (m_Rx.size() < header_len + rem_len) { return; }    // body not fully buffered

			const auto type = static_cast<std::uint8_t>((static_cast<std::uint8_t>(m_Rx[0]) >> 4) & 0x0F);
			const std::string body = m_Rx.substr(header_len, rem_len);
			m_Rx.erase(0, header_len + rem_len);
			Handle(type, body);
		}
	}

	void Handle(std::uint8_t type, const std::string& body)
	{
		auto* self = static_cast<Derived*>(this);
		switch (type)
		{
		case 1:   // CONNECT -> CONNACK (accepted, or refused when rejecting)
			m_SawConnect = true;
			self->SendRaw(Frame(0x20, std::string{ '\x00', static_cast<char>(m_RejectConnect ? 0x05 : 0x00) }));
			break;

		case 3:   // PUBLISH from client (QoS 0) -> record topic
			++m_PublishesReceived;
			if (body.size() >= 2)
			{
				const std::size_t topic_len = (static_cast<std::uint8_t>(body[0]) << 8) | static_cast<std::uint8_t>(body[1]);
				if (body.size() >= 2 + topic_len) { m_LastPublishTopic = body.substr(2, topic_len); }
			}
			break;

		case 8:   // SUBSCRIBE -> SUBACK echoing the packet id, granting QoS 0
			++m_SubscribesReceived;
			if (body.size() >= 2)
			{
				std::string suback;
				suback.push_back(body[0]);
				suback.push_back(body[1]);
				suback.push_back('\x00');
				self->SendRaw(Frame(0x90, suback));
			}
			break;

		case 12:  // PINGREQ -> PINGRESP
			self->SendRaw(Frame(0xD0, std::string{}));
			break;

		case 14:  // DISCONNECT
			m_SawDisconnect = true;
			break;

		default:
			break;
		}
	}

	// Frame a body with byte0 + a remaining-length varint.
	static std::string Frame(std::uint8_t byte0, const std::string& body)
	{
		std::string packet;
		packet.push_back(static_cast<char>(byte0));
		std::size_t len = body.size();
		do
		{
			auto encoded = static_cast<std::uint8_t>(len % 128);
			len /= 128;
			if (len > 0) { encoded |= 0x80; }
			packet.push_back(static_cast<char>(encoded));
		} while (len > 0);
		packet += body;
		return packet;
	}

	bool m_SawConnect{ false };
	bool m_SawDisconnect{ false };
	bool m_RejectConnect{ false };
	std::size_t m_PublishesReceived{ 0 };
	std::size_t m_SubscribesReceived{ 0 };
	std::string m_LastPublishTopic;
	std::string m_Rx;
};

//=============================================================================
// Plain-TCP broker.
//=============================================================================
class MockMqttBroker : public MqttWireCore<MockMqttBroker>
{
public:
	explicit MockMqttBroker(boost::asio::io_context& io_context)
		: m_Acceptor(io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
		, m_Socket(io_context)
	{
		m_Acceptor.async_accept(m_Socket, [this](boost::system::error_code ec) { if (!ec) { DoRead(); } });
	}

	std::uint16_t Port() const { return m_Acceptor.local_endpoint().port(); }

	void DropConnection()
	{
		boost::system::error_code ec;
		m_Socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		m_Socket.close(ec);
	}

	void SendRaw(std::string packet)
	{
		const bool idle = m_WriteQueue.empty();
		m_WriteQueue.push_back(std::move(packet));
		if (idle) { DoWrite(); }
	}

private:
	void DoRead()
	{
		m_Socket.async_read_some(boost::asio::buffer(m_Chunk),
			[this](boost::system::error_code ec, std::size_t n)
			{
				if (ec) { return; }
				FeedBytes(m_Chunk.data(), n);
				DoRead();
			});
	}

	void DoWrite()
	{
		boost::asio::async_write(m_Socket, boost::asio::buffer(m_WriteQueue.front()),
			[this](boost::system::error_code ec, std::size_t)
			{
				if (ec) { return; }
				m_WriteQueue.pop_front();
				if (!m_WriteQueue.empty()) { DoWrite(); }
			});
	}

	boost::asio::ip::tcp::acceptor m_Acceptor;
	boost::asio::ip::tcp::socket m_Socket;
	std::array<char, 1024> m_Chunk{};
	std::deque<std::string> m_WriteQueue;
};


}
// namespace AqualinkAutomate::Test
