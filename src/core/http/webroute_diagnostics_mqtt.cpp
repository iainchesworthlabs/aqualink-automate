#include <source_location>
#include <string>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/server_types.h"
#include "http/webroute_diagnostics_mqtt.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_integration.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Diagnostics_Mqtt::WebRoute_Diagnostics_Mqtt(Kernel::HubLocator& hub_locator) :
		m_HubLocator(hub_locator)
	{
	}

	HTTP::Response WebRoute_Diagnostics_Mqtt::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Mqtt::OnRequest", std::source_location::current());

		if (req.method() != HTTP::Verbs::get)
		{
			return HTTP::Responses::Response_405(req);
		}

		nlohmann::json json;

		auto integration = m_HubLocator.TryFind<Mqtt::MqttIntegration>();
		if (!integration)
		{
			json["enabled"] = false;
			return MakeJsonResponse(req, HTTP::Status::ok, json.dump());
		}

		json["enabled"] = integration->IsEnabled();
		json["running"] = integration->IsRunning();

		auto hub = integration->GetMqttHub();
		if (auto client = hub ? hub->GetMqttClient() : nullptr; client)
		{
			const auto& s = client->Settings();
			json["connected"] = client->IsConnected();
			json["state"] = std::string{ magic_enum::enum_name(client->GetState()) };
			json["broker_host"] = s.broker_host;
			json["broker_port"] = s.broker_port;
			json["tls"] = s.tls.use_tls;
			json["protocol_version"] = std::string{ Options::Mqtt::ToString(s.protocol_version) };
			json["topic_prefix"] = s.topic_prefix;
			json["client_id"] = client->ClientId();
			json["home_assistant_enabled"] = s.home_assistant_enabled;
			json["ha_discovery_prefix"] = s.ha_discovery_prefix;
			json["queue_depth"] = client->PublishQueueDepth();
			json["reconnect_attempts"] = client->ReconnectAttempts();
			json["published"] = client->PublishedCount();
			json["dropped"] = client->DroppedCount();
			json["last_error"] = client->LastError();
		}

		return MakeJsonResponse(req, HTTP::Status::ok, json.dump());
	}

}
// namespace AqualinkAutomate::HTTP
