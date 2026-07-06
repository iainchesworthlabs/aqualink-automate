#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <unordered_set>

#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "options/options_mqtt_options.h"

namespace AqualinkAutomate::Test
{

//=============================================================================
// Friend test seam for the MQTT client: publish-queue inspection and forcing
// the connected state. A single definition shared across all test TUs to avoid
// ODR violations.
//
// (Wire-format / packet-encoding is now owned by the async_mqtt library and is
// no longer hand-rolled here, so the former Encode*/Parse* accessors are gone.)
//=============================================================================

class MqttClientPacketTest
{
public:
	/// Inspect the pending outbound publish queue (topics/payloads/retain) that
	/// the hub / HA-discovery paths enqueued.
	static const std::deque<Mqtt::MqttClient::PendingPublish>& GetPublishQueue(const Mqtt::MqttClient& client)
	{
		return client.m_PublishQueue;
	}

	/// Force the client into the Connected state for integration testing, so the
	/// MqttHub / HA-discovery publish paths run (and enqueue) without a real
	/// broker. The queued publishes can then be inspected via GetPublishQueue().
	static void ForceConnectedState(Mqtt::MqttClient& client)
	{
		client.m_State = Mqtt::MqttClient::State::Connected;
		client.m_Running = true;
	}

	/// Invoke the private client-id generator directly so its format contract
	/// ("aqualink-" + 8 hex chars) can be asserted without going through the ctor.
	static std::string CallGenerateClientId(const Mqtt::MqttClient& client)
	{
		return client.GenerateClientId();
	}

	/// Invoke the private exponential-backoff calculator directly. Combine with
	/// SetReconnectAttempts() to exercise the growth / cap / saturation branches.
	static std::chrono::seconds CallCalculateReconnectDelay(const Mqtt::MqttClient& client)
	{
		return client.CalculateReconnectDelay();
	}

	/// Seed the reconnect-attempt counter so CalculateReconnectDelay()'s backoff
	/// can be driven deterministically across attempt counts.
	static void SetReconnectAttempts(Mqtt::MqttClient& client, std::uint16_t attempts)
	{
		client.m_ReconnectAttempts = attempts;
	}
};

//=============================================================================
// Friend test seam for the MQTT hub: drive the startup retained-topic
// reconciliation (normally signal/timer-gated) directly.
//=============================================================================

class MqttHubReconcileTest
{
public:
	/// Seed the set of retained topics "seen" on the broker during the startup window.
	static void SeedSeenRetainedTopics(Mqtt::MqttHub& hub, const std::unordered_set<std::string>& topics)
	{
		hub.m_RetainedReconcile.SeenTopics = topics;
	}

	/// Invoke the reconciliation pass directly (clears seen topics not owned by the device set).
	static void CallReconcileRetainedTopics(Mqtt::MqttHub& hub)
	{
		hub.ReconcileRetainedTopics();
	}

	/// The set of per-device topics the current device set owns (device JSON + HA state topics).
	static std::unordered_set<std::string> CallComputeOwnedDeviceTopics(const Mqtt::MqttHub& hub)
	{
		return hub.ComputeOwnedDeviceTopics();
	}

	/// Backdate the startup retained-reconcile deadline into the past so the next Poll()
	/// (with the window still pending) runs the one-shot reconcile without a real 10s wait.
	static void ExpireRetainedReconcileDeadline(Mqtt::MqttHub& hub)
	{
		hub.m_RetainedReconcile.Pending = true;
		hub.m_RetainedReconcile.Deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	}
};

//=============================================================================
// Shared test settings factory
//=============================================================================

inline Options::Mqtt::MqttSettings MakeMqttSettings()
{
	Options::Mqtt::MqttSettings s;
	s.enabled = true;
	s.broker_host = "localhost";
	s.broker_port = 1883;
	s.topic_prefix = "test";
	return s;
}

}
// namespace AqualinkAutomate::Test
