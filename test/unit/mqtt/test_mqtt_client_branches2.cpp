#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "mqtt/mqtt_client.h"
#include "options/options_mqtt_options.h"
#include "support/unit_test_mqtt_support.h"

using namespace AqualinkAutomate;

//=============================================================================
// MqttClient branch tests, part 2.
//
// test_mqtt_client_branches.cpp drives the TLS *context* construction (a TLS
// client aimed at a dead port, so the handshake fails deterministically - an
// in-process TLS broker cannot complete a handshake with this client and is not
// attempted). What it never reaches is the endpoint-DISPATCH side of the TLS
// arm: WithEndpoint() picking the TLS endpoint, and DoFlush()/DoSubscribe()
// instantiated for it. Both are exercised here by forcing the facade's
// connected state while a TLS endpoint object exists but its transport does
// not, so every send completes with an error code (async_mqtt refuses to send
// an application packet on a connection whose status is not `connected`).
//
// The mirror arm - the TLS branch taken when the endpoint has already been torn
// down by a reconnect - is covered too: the publish then stays queued.
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

	void PumpSlices(boost::asio::io_context& ioc, int slices)
	{
		for (int i = 0; i < slices; ++i)
		{
			ioc.run_for(std::chrono::milliseconds(5));
		}
	}

	// A loopback port nothing is listening on: bind then immediately release it.
	std::uint16_t AllocateDeadPort(boost::asio::io_context& ioc)
	{
		boost::asio::ip::tcp::acceptor probe(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
		const auto port = probe.local_endpoint().port();
		probe.close();
		return port;
	}

	Options::Mqtt::MqttSettings MakeTlsDeadPortSettings(boost::asio::io_context& ioc, Options::Mqtt::ProtocolVersion version)
	{
		auto settings = Test::MakeMqttSettings();
		settings.broker_host = "127.0.0.1";
		settings.broker_port = AllocateDeadPort(ioc);
		settings.protocol_version = version;
		settings.tls.use_tls = true;
		settings.tls.tls_skip_verify = true;
		settings.reconnect_delay_initial = std::chrono::seconds(5);   // never fires within a test
		settings.reconnect_delay_max = std::chrono::seconds(5);
		return settings;
	}
}

//=============================================================================
// TLS endpoint dispatch (WithEndpoint / DoFlush / DoSubscribe over mqtts)
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_MqttClientBranches2_TlsEndpointDispatch)

// Start() builds the TLS context and endpoint synchronously and leaves the
// underlying handshake in flight. Forcing the connected state at that instant
// means Poll() and Subscribe() dispatch onto the live TLS endpoint: the queue is
// drained into async_send, every send fails (no TLS session), so nothing is
// counted as published and the refused handshake still lands in Reconnecting.
BOOST_AUTO_TEST_CASE(Test_Tls_PublishAndSubscribeOnLiveTlsEndpoint_SendsFailAndNothingIsCounted)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, Options::Mqtt::ProtocolVersion::v3_1_1);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	Test::MqttClientPacketTest::ForceConnectedState(*client);

	client->Publish("pool/state", "on", /*retain=*/true);
	client->Publish("pool/other", "off", /*retain=*/false);
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 2u);

	client->Poll();                                        // DoFlush over the TLS endpoint
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 0u);    // drained into async_send, not into the broker

	BOOST_CHECK_NO_THROW(client->Subscribe("pool/command/#", 0));   // DoSubscribe over the TLS endpoint

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK_EQUAL(client->PublishedCount(), 0u);
	BOOST_CHECK(!client->IsConnected());
	BOOST_CHECK(client->LastError().find("Handshake failed") != std::string::npos);

	client->Stop();
	BOOST_CHECK_EQUAL(static_cast<int>(client->GetState()), static_cast<int>(Mqtt::MqttClient::State::Disconnected));
}

// The same dispatch with MQTT 5 selected, so the v5 publish/subscribe packet arms
// are the ones constructed for the TLS endpoint.
BOOST_AUTO_TEST_CASE(Test_TlsV5_PublishAndSubscribeOnLiveTlsEndpoint_SendsFailAndNothingIsCounted)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, Options::Mqtt::ProtocolVersion::v5);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	Test::MqttClientPacketTest::ForceConnectedState(*client);

	client->Publish("pool/state", "on", /*retain=*/true);
	client->Poll();
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 0u);

	BOOST_CHECK_NO_THROW(client->Subscribe("pool/command/#", 1));

	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));
	BOOST_CHECK_EQUAL(client->PublishedCount(), 0u);
	BOOST_CHECK_EQUAL(client->ReconnectAttempts(), 1u);

	client->Stop();
}

// Once the refused handshake has torn the TLS endpoint down, the TLS arm of
// WithEndpoint() finds no endpoint to dispatch to: a publish issued while the
// facade still believes it is connected stays queued instead of being sent, and
// Stop() then clears it.
BOOST_AUTO_TEST_CASE(Test_Tls_PublishAfterEndpointTornDown_StaysQueued)
{
	boost::asio::io_context ioc;
	auto settings = MakeTlsDeadPortSettings(ioc, Options::Mqtt::ProtocolVersion::v3_1_1);
	auto client = std::make_shared<Mqtt::MqttClient>(ioc, settings);

	client->Start();
	BOOST_REQUIRE(RunUntil(ioc, [&] { return client->GetState() == Mqtt::MqttClient::State::Reconnecting; }));

	Test::MqttClientPacketTest::ForceConnectedState(*client);   // connected state, no live endpoint

	client->Publish("pool/state", "on");
	client->Poll();
	BOOST_CHECK_NO_THROW(client->Subscribe("pool/command/#", 0));
	PumpSlices(ioc, 2);

	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 1u);
	BOOST_CHECK_EQUAL(client->PublishedCount(), 0u);

	client->Stop();
	BOOST_CHECK_EQUAL(client->PublishQueueDepth(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
