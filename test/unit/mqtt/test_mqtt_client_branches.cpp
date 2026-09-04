#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "mqtt/mqtt_client.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"
#include "support/unit_test_mqtt_broker.h"

using namespace AqualinkAutomate;

//=============================================================================
// MqttClient branch tests.
//
// These complement test_mqtt_client.cpp by driving the connection-lifecycle
// arms a happy-path broker session never reaches: the TLS context build + TLS
// endpoint (against a dead port, so the handshake fails deterministically -
// an in-process TLS broker is not attempted), the reconnect timer actually
// firing (zero backoff), Stop() racing in-flight completions, a server that
// closes or speaks garbage before CONNACK, the v5 PUBLISH delivery arm, and
// the remaining CONNECT credential/will permutations.
//=============================================================================

namespace
{
	// Pump the cooperative io_context in short slices until `pred` holds or the
	// iteration budget is exhausted (see test_mqtt_client.cpp).
	template <class Pred>
	bool RunUntil(boost::asio::io_context& ioc, Pred pred)
	{
		for (int i = 0; i < 400; ++i)   // ~400 * 5ms = up to ~2s
		{
			if (pred()) { return true; }
			ioc.run_for(std::chrono::milliseconds(5));
		}
		return pred();
	}

	// Pump a fixed number of slices regardless of state (used after Stop(), when
	// nothing observable is expected to change but late completions must be run).
	void PumpSlices(boost::asio::io_context& ioc, int slices)
	{
		for (int i = 0; i < slices; ++i)
		{
			ioc.run_for(std::chrono::milliseconds(5));
		}
	}

	Options::Mqtt::MqttSettings MakeBrokerSettings(std::uint16_t port)
	{
		auto settings = Test::MakeMqttSettings();
		settings.broker_host = "127.0.0.1";
		settings.broker_port = port;
		return settings;
	}

	// A loopback port nothing is listening on: bind then immediately release it.
	std::uint16_t AllocateDeadPort(boost::asio::io_context& ioc)
	{
		boost::asio::ip::tcp::acceptor probe(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
		const auto port = probe.local_endpoint().port();
		probe.close();
		return port;
	}

	Options::Mqtt::MqttSettings MakeTlsDeadPortSettings(boost::asio::io_context& ioc, bool skip_verify)
	{
		auto settings = MakeBrokerSettings(AllocateDeadPort(ioc));
		settings.tls.use_tls = true;
		settings.tls.tls_skip_verify = skip_verify;
		settings.reconnect_delay_initial = std::chrono::seconds(5);   // never fires within a test
		settings.reconnect_delay_max = std::chrono::seconds(5);
		return settings;
	}

	// A raw TCP acceptor that misbehaves in a scripted way once a client connects:
	// either closes the socket straight away, or pushes a pre-canned packet and
	// then holds the connection open (never answering the CONNECT).
	class RawTcpServer
	{
	public:
		enum class Behaviour
		{
			CloseImmediately,
			SendThenHold
		};

		RawTcpServer(boost::asio::io_context& ioc, Behaviour behaviour, std::string bytes_to_send = {})
			: m_Acceptor(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
			, m_Socket(ioc)
			, m_Behaviour(behaviour)
			, m_BytesToSend(std::move(bytes_to_send))
		{
			m_Acceptor.async_accept(m_Socket, [this](boost::system::error_code ec) { OnAccept(ec); });
		}

		std::uint16_t Port() const { return m_Acceptor.local_endpoint().port(); }
		bool Accepted() const { return m_Accepted; }

	private:
		void OnAccept(boost::system::error_code ec)
		{
			if (ec) { return; }
			m_Accepted = true;

			if (Behaviour::CloseImmediately == m_Behaviour)
			{
				boost::system::error_code ignored;
				m_Socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
				m_Socket.close(ignored);
				return;
			}

			boost::asio::async_write(m_Socket, boost::asio::buffer(m_BytesToSend),
				[this](boost::system::error_code write_ec, std::size_t)
				{
					if (write_ec) { return; }
					HoldOpen();
				});
		}

		// Keep a read pending so the socket stays open (and the io_context stays busy)
		// until the client gives up on it.
		void HoldOpen()
		{
			m_Socket.async_read_some(boost::asio::buffer(m_Chunk),
				[this](boost::system::error_code read_ec, std::size_t)
				{
					if (read_ec) { return; }
					HoldOpen();
				});
		}

		boost::asio::ip::tcp::acceptor m_Acceptor;
		boost::asio::ip::tcp::socket m_Socket;
		Behaviour m_Behaviour;
		std::string m_BytesToSend;
		std::array<char, 256> m_Chunk{};
		bool m_Accepted{ false };
	};

	// MQTT 3.1.1 QoS-0 PUBLISH wire form (fixed header 0x30, remaining-length byte,
	// 2-byte topic length, topic, payload) - short enough for a 1-byte length.
	std::string EncodeQos0Publish(const std::string& topic, const std::string& payload)
	{
		std::string body;
		body.push_back(static_cast<char>((topic.size() >> 8) & 0xFF));
		body.push_back(static_cast<char>(topic.size() & 0xFF));
		body += topic;
		body += payload;

		std::string packet;
		packet.push_back('\x30');
		packet.push_back(static_cast<char>(body.size()));
		packet += body;
		return packet;
	}

	// A throw-away file under the OS temp directory, deleted on scope exit.
	class TempFile
	{
	public:
		TempFile(const std::string& name, const std::string& content)
			: m_Path(std::filesystem::temp_directory_path() / name)
		{
			std::ofstream out(m_Path, std::ios::binary | std::ios::trunc);
			out << content;
		}

		~TempFile()
		{
			std::error_code ec;
			std::filesystem::remove(m_Path, ec);
		}

		std::string Path() const { return m_Path.string(); }

	private:
		std::filesystem::path m_Path;
	};

	// A self-signed EC (P-256) certificate + matching key, generated once for these
	// tests (CN=mqtt-unit-test, 100-year validity). Only ever loaded into a client
	// SSL context that then fails to connect - never used to authenticate anything.
	constexpr const char* TEST_CERT_PEM =
		"-----BEGIN CERTIFICATE-----\n"
		"MIIBiTCCAS+gAwIBAgIUPyajQLc4fPJXSd/DYhlq/ctzGY0wCgYIKoZIzj0EAwIw\n"
		"GTEXMBUGA1UEAwwObXF0dC11bml0LXRlc3QwIBcNMjYwOTA0MDAzNDI2WhgPMjEy\n"
		"NjA4MTEwMDM0MjZaMBkxFzAVBgNVBAMMDm1xdHQtdW5pdC10ZXN0MFkwEwYHKoZI\n"
		"zj0CAQYIKoZIzj0DAQcDQgAEy/79+0P2162MJL9b2qUjDS2CmZL5LrjUz424qoq+\n"
		"Fl3jnSuqqyaHU2jc7F5L2W7qq8gSka8FrXz8zNBfYcXXZqNTMFEwHQYDVR0OBBYE\n"
		"FBulqrpOLpDYOD4XIcCQW/KuBHovMB8GA1UdIwQYMBaAFBulqrpOLpDYOD4XIcCQ\n"
		"W/KuBHovMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgNHJtmuC4\n"
		"9tBZ94hP8pWRHpg2aj0ru8TbAlMOQEnD/yUCIQCZaTiKP+iqyMbvA3IaiZPgca6F\n"
		"m8x6JiR4+AlRVFfHMw==\n"
		"-----END CERTIFICATE-----\n";

	constexpr const char* TEST_KEY_PEM =
		"-----BEGIN PRIVATE KEY-----\n"
		"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgR2754Nu4Kx6wnfEV\n"
		"vFkzziJe6u22dLFJQXFD6AE7ym2hRANCAATL/v37Q/bXrYwkv1vapSMNLYKZkvku\n"
		"uNTPjbiqir4WXeOdK6qrJodTaNzsXkvZbuqryBKRrwWtfPzM0F9hxddm\n"
		"-----END PRIVATE KEY-----\n";

	constexpr const char* NOT_A_PEM = "this is not a PEM document\n";
}

//=============================================================================
// Ownership / construction arms
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches_Ownership)

// BeginConnect() refuses to start an endpoint for a client that is not owned by a
// shared_ptr (the async chain needs shared_from_this): a stack client stays in
// Connecting with nothing in flight, and Stop() still tears it down cleanly.
BOOST_AUTO_TEST_CASE(Test_Start_OnStackClient_StaysConnectingWithoutEndpoint)
{
	boost::asio::io_context ioc;
	Mqtt::MqttClient client(ioc, Test::MakeMqttSettings());

	client.Start();
	BOOST_CHECK(client.IsRunning());
	BOOST_CHECK_EQUAL(static_cast<int>(client.GetState()), static_cast<int>(Mqtt::MqttClient::State::Connecting));

	// Nothing was scheduled, so pumping changes nothing and Poll() is a no-op flush.
	BOOST_CHECK_NO_THROW(client.Poll());
	ioc.poll();
	BOOST_CHECK_EQUAL(static_cast<int>(client.GetState()), static_cast<int>(Mqtt::MqttClient::State::Connecting));
	BOOST_CHECK(!client.IsConnected());
	BOOST_CHECK_EQUAL(client.ReconnectAttempts(), 0u);

	client.Stop();
	BOOST_CHECK(!client.IsRunning());
	BOOST_CHECK_EQUAL(static_cast<int>(client.GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
}

// A password configured WITH TLS takes the ctor's "no cleartext warning" arm.
BOOST_AUTO_TEST_CASE(Test_Construction_PasswordWithTls_Succeeds)
{
	boost::asio::io_context ioc;
	auto settings = Test::MakeMqttSettings();
	settings.username = "pooluser";
	settings.password = "poolpass";
	settings.tls.use_tls = true;

	Mqtt::MqttClient client(ioc, settings);

	BOOST_CHECK(client.Settings().tls.use_tls);
	BOOST_CHECK_EQUAL(client.Settings().password, "poolpass");
	BOOST_CHECK(!client.IsRunning());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// TLS endpoint + SSL-context build. The TLS handshake itself cannot complete
// in-process, so every case points the client at a dead loopback port: the
// context is built (all of its option arms), the TLS endpoint + SNI + verify
// callback are configured, the underlying handshake is refused, and the client
// tears the TLS endpoint down and schedules a reconnect.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches_Tls)

BOOST_AUTO_TEST_CASE(Test_Tls_SkipVerify_DeadPort_SchedulesReconnect)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/true);
	// A client certificate WITHOUT a key: LoadClientCertificate bails before touching files.
	settings.tls.tls_client_cert = "client-cert-without-key.pem";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	std::vector<std::string> errors;
	client->OnError.connect([&](const std::string& message) { errors.push_back(message); });

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);
	BOOST_CHECK(client->LastError().find("Handshake failed") != std::string::npos);
	BOOST_REQUIRE_EQUAL(errors.size(), 1u);
	BOOST_CHECK(errors[0].find("Underlying handshake failed") != std::string::npos);

	client->Stop();
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
}

// verify_peer with every certificate path pointing at a file that does not exist:
// the CA "not found" arm, the client cert/key "not found" arm, and the hostname
// verification callback install all run before the refused handshake.
BOOST_AUTO_TEST_CASE(Test_Tls_VerifyPeer_MissingCertFiles_SchedulesReconnect)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/false);
	settings.tls.tls_ca_cert = "does-not-exist-ca.pem";
	settings.tls.tls_client_cert = "does-not-exist-client.pem";
	settings.tls.tls_client_key = "does-not-exist-client-key.pem";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);
	BOOST_CHECK(!client->LastError().empty());

	client->Stop();
}

// Well-formed CA + client certificate + key files load successfully into the
// context (the ec-clear arms) before the refused handshake.
BOOST_AUTO_TEST_CASE(Test_Tls_ValidCaAndClientCertFiles_LoadAndScheduleReconnect)
{
	boost::asio::io_context ioc;
	TempFile ca("aqualink-mqtt-test-ca.pem", TEST_CERT_PEM);
	TempFile cert("aqualink-mqtt-test-client.pem", TEST_CERT_PEM);
	TempFile key("aqualink-mqtt-test-client-key.pem", TEST_KEY_PEM);

	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/false);
	settings.tls.tls_ca_cert = ca.Path();
	settings.tls.tls_client_cert = cert.Path();
	settings.tls.tls_client_key = key.Path();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK(client->LastError().find("Handshake failed") != std::string::npos);

	client->Stop();
}

// Files that exist but are not PEM: the CA load error arm and the client
// certificate load error arm (which returns before the key is attempted).
BOOST_AUTO_TEST_CASE(Test_Tls_MalformedCaAndClientCert_LogsLoadErrorsAndSchedulesReconnect)
{
	boost::asio::io_context ioc;
	TempFile ca("aqualink-mqtt-test-bad-ca.pem", NOT_A_PEM);
	TempFile cert("aqualink-mqtt-test-bad-client.pem", NOT_A_PEM);
	TempFile key("aqualink-mqtt-test-bad-client-key.pem", TEST_KEY_PEM);

	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/true);
	settings.tls.tls_ca_cert = ca.Path();
	settings.tls.tls_client_cert = cert.Path();
	settings.tls.tls_client_key = key.Path();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);

	client->Stop();
}

// A valid client certificate whose key file is garbage: the certificate loads,
// then the private-key load error arm runs.
BOOST_AUTO_TEST_CASE(Test_Tls_ValidClientCertWithMalformedKey_LogsKeyErrorAndSchedulesReconnect)
{
	boost::asio::io_context ioc;
	TempFile cert("aqualink-mqtt-test-good-client.pem", TEST_CERT_PEM);
	TempFile key("aqualink-mqtt-test-bad-key.pem", NOT_A_PEM);

	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/true);
	settings.tls.tls_client_cert = cert.Path();
	settings.tls.tls_client_key = key.Path();
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(!client->IsConnected());

	client->Stop();
}

// Stopping a TLS client while it is Reconnecting cancels the backoff timer and
// tears down without ever re-entering BeginConnect.
BOOST_AUTO_TEST_CASE(Test_Tls_Stop_WhileReconnecting_StaysDisconnected)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, /*skip_verify=*/true);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));

	client->Stop();
	PumpSlices(ioc, 10);

	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);
	BOOST_CHECK(!client->IsRunning());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Reconnect timer + Stop() racing in-flight completions
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches_Reconnect)

// With a zero backoff the reconnect timer fires immediately, re-entering
// BeginConnect for another (again refused) attempt: the attempt counter climbs
// past the first failure and the error signal fires once per attempt.
BOOST_AUTO_TEST_CASE(Test_Reconnect_ZeroBackoff_RetriesAfterRefusedHandshake)
{
	boost::asio::io_context ioc;
	auto settings = MakeBrokerSettings(AllocateDeadPort(ioc));
	settings.reconnect_delay_initial = std::chrono::seconds(0);
	settings.reconnect_delay_max = std::chrono::seconds(0);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	std::size_t error_count = 0;
	client->OnError.connect([&](const std::string&) { ++error_count; });

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->ReconnectAttempts() >= 3; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_GE(error_count, 3u);
	BOOST_CHECK(!client->LastError().empty());

	client->Stop();
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
}

// After the broker drops an established session, a zero backoff makes the client
// reconnect straight away: it emits the disconnect, then re-enters Connecting for
// its second attempt (the mock broker only ever accepts one session, so the retry
// parks in Connecting waiting for a CONNACK that never comes).
BOOST_AUTO_TEST_CASE(Test_Reconnect_ZeroBackoff_AfterBrokerDrop_ReentersConnecting)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto settings = MakeBrokerSettings(broker.Port());
	settings.reconnect_delay_initial = std::chrono::seconds(0);
	settings.reconnect_delay_max = std::chrono::seconds(0);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	std::vector<std::string> disconnect_reasons;
	client->OnDisconnected.connect([&](const std::string& reason) { disconnect_reasons.push_back(reason); });

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));

	broker.DropConnection();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Connecting && client->ReconnectAttempts() >= 1; }));
	BOOST_REQUIRE_EQUAL(disconnect_reasons.size(), 1u);
	BOOST_CHECK(disconnect_reasons[0].find("Connection lost") != std::string::npos);
	BOOST_CHECK(!client->IsConnected());

	client->Stop();
	// Stop() emits its own disconnect on top of the connection-lost one.
	BOOST_CHECK_EQUAL(disconnect_reasons.size(), 2u);
}

// Stop() during the backoff window cancels the reconnect timer: its completion
// runs with operation_aborted and must not re-enter BeginConnect.
BOOST_AUTO_TEST_CASE(Test_Stop_WhileReconnecting_CancelsBackoffTimer)
{
	boost::asio::io_context ioc;
	auto settings = MakeBrokerSettings(AllocateDeadPort(ioc));
	settings.reconnect_delay_initial = std::chrono::seconds(5);
	settings.reconnect_delay_max = std::chrono::seconds(5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);

	client->Stop();
	PumpSlices(ioc, 10);

	BOOST_CHECK(!client->IsRunning());
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);   // no further attempt was made
}

// Stop() before the handshake completes: the late handshake completion sees the
// client is no longer running and bows out without touching state.
BOOST_AUTO_TEST_CASE(Test_Stop_WhileConnecting_LateHandshakeCompletionIsIgnored)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, MakeBrokerSettings(broker.Port()));

	std::size_t disconnect_count = 0;
	client->OnDisconnected.connect([&](const std::string&) { ++disconnect_count; });

	client->Start();
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Connecting));
	client->Stop();   // nothing has been pumped yet: the resolve/connect is still in flight

	PumpSlices(ioc, 10);

	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 0u);
	BOOST_CHECK(client->LastError().empty());
	BOOST_CHECK_EQUAL(disconnect_count, 1u);   // only Stop()'s own "Client stopped"
}

// Stop() on an established session: the receive loop's completion (cancelled by
// the close) must not be mistaken for a connection loss.
BOOST_AUTO_TEST_CASE(Test_Stop_WhileConnected_LateRecvCompletionIsIgnored)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, MakeBrokerSettings(broker.Port()));

	std::vector<std::string> disconnect_reasons;
	client->OnDisconnected.connect([&](const std::string& reason) { disconnect_reasons.push_back(reason); });

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));

	client->Stop();
	PumpSlices(ioc, 10);

	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 0u);
	BOOST_CHECK(client->LastError().empty());
	BOOST_REQUIRE_EQUAL(disconnect_reasons.size(), 1u);
	BOOST_CHECK_EQUAL(disconnect_reasons[0], "Client stopped");
}

// A publish issued after the broker dropped the socket but before the receive
// loop has observed it: the flush sends on a dead connection (its completion
// takes either the send-error or the counted arm), then the loss is detected.
BOOST_AUTO_TEST_CASE(Test_Publish_AfterBrokerDrop_BeforeLossObserved_DoesNotThrow)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto settings = MakeBrokerSettings(broker.Port());
	settings.reconnect_delay_initial = std::chrono::seconds(5);
	settings.reconnect_delay_max = std::chrono::seconds(5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	bool disconnected = false;
	client->OnDisconnected.connect([&](const std::string&) { disconnected = true; });

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));

	broker.DropConnection();
	BOOST_CHECK(client->IsConnected());   // the loss has not been pumped through yet

	BOOST_CHECK_NO_THROW(client->Publish("pool/late", "1"));
	BOOST_CHECK_NO_THROW(client->Poll());

	BOOST_REQUIRE(RunUntil(ioc, [&] { return disconnected; }));
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Reconnecting));
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 0u);   // the item was handed to the endpoint, not requeued

	client->Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Servers that misbehave before CONNACK
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches_BadServers)

// The TCP connect succeeds but the peer closes straight away: either the CONNECT
// send or the CONNACK receive fails, and both arms schedule a reconnect.
BOOST_AUTO_TEST_CASE(Test_ServerClosesBeforeConnack_SchedulesReconnect)
{
	boost::asio::io_context ioc;
	RawTcpServer server(ioc, RawTcpServer::Behaviour::CloseImmediately);
	auto settings = MakeBrokerSettings(server.Port());
	settings.reconnect_delay_initial = std::chrono::seconds(5);
	settings.reconnect_delay_max = std::chrono::seconds(5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(server.Accepted());
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);
	BOOST_CHECK_MESSAGE(client->LastError().find("CONN") != std::string::npos,
		"expected a CONNECT-send or CONNACK-recv failure, got: " + client->LastError());

	client->Stop();
}

// A server that pushes a PUBLISH before any CONNACK: the protocol layer rejects
// the out-of-order packet, which surfaces as a CONNACK receive error.
BOOST_AUTO_TEST_CASE(Test_ServerSendsPublishBeforeConnack_SchedulesReconnect)
{
	boost::asio::io_context ioc;
	RawTcpServer server(ioc, RawTcpServer::Behaviour::SendThenHold, EncodeQos0Publish("pool/rogue", "1"));
	auto settings = MakeBrokerSettings(server.Port());
	settings.reconnect_delay_initial = std::chrono::seconds(5);
	settings.reconnect_delay_max = std::chrono::seconds(5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	std::string received_topic;
	client->OnMessageReceived.connect([&](const std::string& topic, const std::string&) { received_topic = topic; });

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK(server.Accepted());
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK(received_topic.empty());   // never delivered as an application message
	BOOST_CHECK(client->LastError().find("CONNACK") != std::string::npos);

	client->Stop();
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Remaining protocol / CONNECT permutations against the mock broker
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches_Permutations)

// MQTT 5 PUBLISH delivery: a v5 PUBLISH carries a property-length varint between
// the topic and the payload (a single 0x00 here), so the v5 publish_packet arm of
// the receive loop is what unpacks it into the MessageReceived signal.
BOOST_AUTO_TEST_CASE(Test_ProtocolV5_BrokerPublish_DeliversV5PublishPacket)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto settings = MakeBrokerSettings(broker.Port());
	settings.protocol_version = Options::Mqtt::ProtocolVersion::v5;
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	std::string got_topic, got_payload;
	client->OnMessageReceived.connect([&](const std::string& t, const std::string& p) { got_topic = t; got_payload = p; });

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));

	// Empty v5 property set (0x00) followed by the application payload.
	broker.PublishToClient("pool/command/pump", std::string("\x00", 1) + "ON");

	BOOST_REQUIRE(RunUntil(ioc, [&] { return !got_topic.empty(); }));
	BOOST_CHECK_EQUAL(got_topic, "pool/command/pump");
	BOOST_CHECK_EQUAL(got_payload, "ON");

	client->Stop();
}

// A last-will WITHOUT retain takes the non-retained will-options arm of DoSendConnect.
BOOST_AUTO_TEST_CASE(Test_Connect_WillWithoutRetain_ReachesConnected)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, MakeBrokerSettings(broker.Port()));

	client->SetWill("status/availability", "offline", /*retain=*/false);
	BOOST_REQUIRE(client->GetWill().has_value());
	BOOST_CHECK(!client->GetWill()->retain);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));
	BOOST_CHECK(broker.ClientConnected());

	client->Stop();
}

// A username with NO password: the user is sent, the password arm is skipped.
BOOST_AUTO_TEST_CASE(Test_Connect_UsernameWithoutPassword_ReachesConnected)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto settings = MakeBrokerSettings(broker.Port());
	settings.username = "pooluser";
	settings.password = "";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));
	BOOST_CHECK(broker.ClientConnected());

	client->Stop();
}

// v5 CONNECT with will + credentials, then a v5 subscribe and a retained v5 publish
// in the same session (the v5 packet arms with every optional field populated).
BOOST_AUTO_TEST_CASE(Test_ProtocolV5_WillCredentialsSubscribeAndRetainedPublish)
{
	boost::asio::io_context ioc;
	Test::MockMqttBroker broker(ioc);
	auto settings = MakeBrokerSettings(broker.Port());
	settings.protocol_version = Options::Mqtt::ProtocolVersion::v5;
	settings.username = "pooluser";
	settings.password = "poolpass";
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);
	client->SetWill("status/availability", "offline", /*retain=*/true);

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->IsConnected(); }));

	client->Subscribe("pool/command/#", 1);
	client->Publish("pool/state", "on", /*retain=*/true);
	client->Poll();

	BOOST_REQUIRE(RunUntil(ioc, [&] { return broker.SubscribesReceived() >= 1 && broker.PublishesReceived() >= 1; }));
	BOOST_CHECK_EQUAL(broker.LastPublishTopic(), "pool/state");
	BOOST_CHECK_GE(client->PublishedCount(), 1u);
	BOOST_CHECK(client->IsConnected());

	client->Stop();
}

// While Reconnecting, publishes queue (no flush) and subscribes are refused.
BOOST_AUTO_TEST_CASE(Test_WhileReconnecting_PublishQueuesAndSubscribeIsRefused)
{
	boost::asio::io_context ioc;
	auto settings = MakeBrokerSettings(AllocateDeadPort(ioc));
	settings.reconnect_delay_initial = std::chrono::seconds(5);
	settings.reconnect_delay_max = std::chrono::seconds(5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));

	client->Publish("pool/a", "1");
	client->Publish("pool/b", "2");
	client->Poll();   // FlushIfConnected: not connected -> nothing drained
	BOOST_CHECK_NO_THROW(client->Subscribe("pool/command/#", 0));
	PumpSlices(ioc, 2);

	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 2u);
	BOOST_CHECK_EQUAL(client->PublishedCount(), 0u);

	client->Stop();
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 0u);   // Shutdown clears the queue
}

BOOST_AUTO_TEST_SUITE_END()
