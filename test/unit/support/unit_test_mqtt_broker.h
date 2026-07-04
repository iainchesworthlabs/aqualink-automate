#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

namespace AqualinkAutomate::Test
{

//=============================================================================
// MockMqttBroker — a minimal in-process MQTT 3.1.1 broker over loopback TCP.
//
// It exists purely so the real MqttClient can complete an actual connection
// (TCP handshake -> CONNECT/CONNACK -> recv loop / publish flush / subscribe /
// broker-pushed PUBLISH / disconnect-reconnect) against something on the same
// io_context, exercising the async connection code that a broker-less client
// never reaches. It is NOT a conforming broker: it speaks exactly the handful
// of packets the client sends and replies with the minimum the client needs.
//
// Cooperative + single-threaded: construct it with the SAME io_context as the
// client, then pump that context (run_for/poll) — the acceptor, the client's
// connect and the broker's reads all advance together on one thread.
//
// Wire format (MQTT 3.1.1 fixed header): byte0 = type<<4 | flags, followed by a
// 1–4 byte varint "remaining length", then that many bytes of body.
//=============================================================================
class MockMqttBroker
{
public:
	explicit MockMqttBroker(boost::asio::io_context& io_context)
		: m_Acceptor(io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
		, m_Socket(io_context)
	{
		DoAccept();
	}

	// The OS-assigned ephemeral port the client should connect to.
	std::uint16_t Port() const { return m_Acceptor.local_endpoint().port(); }

	// Observation accessors for assertions.
	bool ClientConnected() const { return m_SawConnect; }
	bool ClientDisconnected() const { return m_SawDisconnect; }
	std::size_t PublishesReceived() const { return m_PublishesReceived; }
	std::size_t SubscribesReceived() const { return m_SubscribesReceived; }
	const std::string& LastPublishTopic() const { return m_LastPublishTopic; }

	// Push a QoS-0 PUBLISH to the connected client, driving its recv loop and the
	// MessageReceived signal.
	void PublishToClient(const std::string& topic, const std::string& payload)
	{
		std::string body;
		body.push_back(static_cast<char>((topic.size() >> 8) & 0xFF));
		body.push_back(static_cast<char>(topic.size() & 0xFF));
		body += topic;
		body += payload;   // QoS 0 => no packet identifier
		SendPacket(0x30, body);
	}

	// Drop the client connection so the client's recv loop observes a read error
	// and drives its disconnect/reconnect path.
	void DropConnection()
	{
		boost::system::error_code ec;
		m_Socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		m_Socket.close(ec);
	}

private:
	void DoAccept()
	{
		m_Acceptor.async_accept(m_Socket, [this](boost::system::error_code ec)
		{
			if (!ec) { DoRead(); }
		});
	}

	void DoRead()
	{
		m_Socket.async_read_some(boost::asio::buffer(m_ReadChunk),
			[this](boost::system::error_code ec, std::size_t n)
			{
				if (ec) { return; }
				m_Rx.append(m_ReadChunk.data(), n);
				ParseAll();
				DoRead();
			});
	}

	// Parse and dispatch every complete packet currently buffered in m_Rx.
	void ParseAll()
	{
		for (;;)
		{
			if (m_Rx.size() < 2) { return; }

			// Decode the remaining-length varint that follows byte0.
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
			if (!complete_len) { return; }              // varint not fully buffered yet
			if (m_Rx.size() < header_len + rem_len) { return; }   // body not fully buffered yet

			const auto type = static_cast<std::uint8_t>((static_cast<std::uint8_t>(m_Rx[0]) >> 4) & 0x0F);
			const std::string body = m_Rx.substr(header_len, rem_len);
			m_Rx.erase(0, header_len + rem_len);

			Handle(type, body);
		}
	}

	void Handle(std::uint8_t type, const std::string& body)
	{
		switch (type)
		{
		case 1:   // CONNECT -> accept
			m_SawConnect = true;
			SendPacket(0x20, std::string{ '\x00', '\x00' });   // CONNACK: session-present 0, return code 0 (accepted)
			break;

		case 3:   // PUBLISH from client (QoS 0 in this client) -> record the topic
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
				suback.push_back(body[0]);   // packet id MSB
				suback.push_back(body[1]);   // packet id LSB
				suback.push_back('\x00');    // granted QoS 0
				SendPacket(0x90, suback);
			}
			break;

		case 12:  // PINGREQ -> PINGRESP
			SendPacket(0xD0, std::string{});
			break;

		case 14:  // DISCONNECT
			m_SawDisconnect = true;
			break;

		default:
			break;
		}
	}

	// Frame `body` with the given fixed-header byte0 and a remaining-length
	// varint, then write it to the client (serialised through a small queue so
	// concurrent sends never interleave on the wire).
	void SendPacket(std::uint8_t byte0, const std::string& body)
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

		const bool idle = m_WriteQueue.empty();
		m_WriteQueue.push_back(std::move(packet));
		if (idle) { DoWrite(); }
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
	std::array<char, 1024> m_ReadChunk{};
	std::string m_Rx;
	std::deque<std::string> m_WriteQueue;

	bool m_SawConnect{ false };
	bool m_SawDisconnect{ false };
	std::size_t m_PublishesReceived{ 0 };
	std::size_t m_SubscribesReceived{ 0 };
	std::string m_LastPublishTopic;
};

}
// namespace AqualinkAutomate::Test
