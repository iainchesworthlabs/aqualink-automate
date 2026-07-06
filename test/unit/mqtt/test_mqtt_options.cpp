#include <boost/test/unit_test.hpp>

#include <boost/any.hpp>
#include <boost/program_options.hpp>

#include "options/options_mqtt_options.h"
#include "exceptions/exception_options_missingdependency.h"

using namespace AqualinkAutomate;
namespace po = boost::program_options;

namespace AqualinkAutomate::Test
{
	/// Helper to create a variables_map from simulated command line arguments using
	/// the MQTT OptionsProcessor's options description.
	po::variables_map ParseMqttOptions(Options::Mqtt::OptionsProcessor& processor, const std::vector<const char*>& args)
	{
		po::options_description desc;
		desc.add(processor.Options());

		po::variables_map vm;
		po::store(po::parse_command_line(static_cast<int>(args.size()), args.data(), desc), vm);
		po::notify(vm);

		return vm;
	}
}
// namespace AqualinkAutomate::Test

BOOST_AUTO_TEST_SUITE(TestSuite_MqttOptions)

//-----------------------------------------------------------------------------
// DEFAULT VALUES
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_DefaultSettings)
{
	Options::Mqtt::MqttSettings settings;

	BOOST_CHECK_EQUAL(settings.enabled, false);
	BOOST_CHECK_EQUAL(settings.broker_host, "localhost");
	BOOST_CHECK_EQUAL(settings.broker_port, 1883);
	BOOST_CHECK_EQUAL(settings.tls.use_tls, false);
	BOOST_CHECK(settings.tls.tls_ca_cert.empty());
	BOOST_CHECK(settings.tls.tls_client_cert.empty());
	BOOST_CHECK(settings.tls.tls_client_key.empty());
	BOOST_CHECK_EQUAL(settings.tls.tls_skip_verify, false);
	BOOST_CHECK(settings.client_id.empty());
	BOOST_CHECK(settings.username.empty());
	BOOST_CHECK(settings.password.empty());
	BOOST_CHECK_EQUAL(settings.topic_prefix, "aqualink");
	BOOST_CHECK_EQUAL(settings.status_publish_interval.count(), 5);
	BOOST_CHECK_EQUAL(settings.statistics_publish_interval.count(), 10);
	BOOST_CHECK_EQUAL(settings.publish_on_change, true);
	BOOST_CHECK_EQUAL(settings.home_assistant_enabled, false);
	BOOST_CHECK_EQUAL(settings.ha_discovery_prefix, "homeassistant");
	BOOST_CHECK(settings.ha_device_id.empty());
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProcessDefaults)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	const auto& settings = result.value();

	BOOST_CHECK_EQUAL(settings.enabled, false);
	BOOST_CHECK_EQUAL(settings.broker_host, "localhost");
	BOOST_CHECK_EQUAL(settings.broker_port, 1883);
	BOOST_CHECK_EQUAL(settings.tls.use_tls, false);
	BOOST_CHECK_EQUAL(settings.topic_prefix, "aqualink");
	BOOST_CHECK_EQUAL(settings.home_assistant_enabled, false);
	BOOST_CHECK_EQUAL(settings.ha_discovery_prefix, "homeassistant");
	BOOST_CHECK_EQUAL(settings.ha_device_id, "aqualink_aqualink");
}

//-----------------------------------------------------------------------------
// ENABLE MQTT
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_EnableMqtt)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().enabled, true);
}

//-----------------------------------------------------------------------------
// BROKER CONFIGURATION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_BrokerHost)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-host=broker.example.com" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().broker_host, "broker.example.com");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_BrokerPort)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-port=8883" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().broker_port, 8883);
}

//-----------------------------------------------------------------------------
// TOPIC CONFIGURATION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TopicPrefix)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-prefix=mypool" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().topic_prefix, "mypool");
}

//-----------------------------------------------------------------------------
// AUTHENTICATION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_Authentication)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-username=testuser", "--mqtt-password=testpass" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().username, "testuser");
	BOOST_CHECK_EQUAL(result.value().password, "testpass");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ClientId)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-client-id=pool-ctrl-001" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().client_id, "pool-ctrl-001");
}

//-----------------------------------------------------------------------------
// TLS CONFIGURATION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsEnabled)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-tls" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().tls.use_tls, true);
	// When TLS is enabled and port is not explicitly set, should auto-switch to 8883
	BOOST_CHECK_EQUAL(result.value().broker_port, 8883);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsWithExplicitPort)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-tls", "--mqtt-port=9883" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().tls.use_tls, true);
	// Explicitly set port should be kept
	BOOST_CHECK_EQUAL(result.value().broker_port, 9883);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsCertificates)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-tls",
		"--mqtt-ca-cert=/path/to/ca.pem",
		"--mqtt-client-cert=/path/to/cert.pem",
		"--mqtt-client-key=/path/to/key.pem" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().tls.tls_ca_cert, "/path/to/ca.pem");
	BOOST_CHECK_EQUAL(result.value().tls.tls_client_cert, "/path/to/cert.pem");
	BOOST_CHECK_EQUAL(result.value().tls.tls_client_key, "/path/to/key.pem");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsSkipVerify)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-tls", "--mqtt-tls-skip-verify" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().tls.tls_skip_verify, true);
}

//-----------------------------------------------------------------------------
// TLS VALIDATION - CLIENT CERT REQUIRES KEY
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsCertWithoutKey)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-client-cert=/path/to/cert.pem" });

	BOOST_CHECK_THROW(processor.Validate(vm), Exceptions::Options_MissingDependency);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsKeyWithoutCert)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-client-key=/path/to/key.pem" });

	BOOST_CHECK_THROW(processor.Validate(vm), Exceptions::Options_MissingDependency);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_TlsCertAndKeyTogether)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt",
		"--mqtt-client-cert=/path/to/cert.pem",
		"--mqtt-client-key=/path/to/key.pem" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));
}

//-----------------------------------------------------------------------------
// PUBLISHING INTERVALS
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_StatusInterval)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-status-interval=15" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().status_publish_interval.count(), 15);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_StatsInterval)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--mqtt-stats-interval=120" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().statistics_publish_interval.count(), 120);
}

//-----------------------------------------------------------------------------
// HOME ASSISTANT OPTIONS
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaEnabled)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--home-assistant" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().home_assistant_enabled, true);
	BOOST_CHECK_EQUAL(result.value().ha_discovery_prefix, "homeassistant");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaCustomPrefix)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--home-assistant", "--ha-discovery-prefix=ha_test" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().ha_discovery_prefix, "ha_test");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaRequiresMqtt)
{
	Options::Mqtt::OptionsProcessor processor;
	// --home-assistant without --mqtt should fail validation
	auto vm = Test::ParseMqttOptions(processor, { "program", "--home-assistant" });

	BOOST_CHECK_THROW(processor.Validate(vm), Exceptions::Options_MissingDependency);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaDeviceId)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--home-assistant", "--ha-device-id=custom_id" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().ha_device_id, "custom_id");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaDeviceIdDefaultDerivation)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--home-assistant", "--mqtt-prefix=mypool" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	// Empty ha_device_id should be derived from topic_prefix
	BOOST_CHECK_EQUAL(result.value().ha_device_id, "aqualink_mypool");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_HaDeviceIdRequiresHa)
{
	Options::Mqtt::OptionsProcessor processor;
	// --ha-device-id without --home-assistant should fail validation
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt", "--ha-device-id=test_id" });

	BOOST_CHECK_THROW(processor.Validate(vm), Exceptions::Options_MissingDependency);
}

//-----------------------------------------------------------------------------
// COMPLETE CONFIGURATION
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_CompleteConfiguration)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program",
		"--mqtt",
		"--mqtt-host=secure.broker.com",
		"--mqtt-port=8883",
		"--mqtt-tls",
		"--mqtt-ca-cert=/etc/ssl/certs/ca.pem",
		"--mqtt-client-cert=/etc/ssl/certs/client.pem",
		"--mqtt-client-key=/etc/ssl/private/client.key",
		"--mqtt-client-id=pool-controller-001",
		"--mqtt-username=pool_user",
		"--mqtt-password=secure_password",
		"--mqtt-prefix=home/pool",
		"--mqtt-status-interval=15",
		"--mqtt-stats-interval=120",
		"--home-assistant",
		"--ha-discovery-prefix=ha_custom",
		"--ha-device-id=pool-controller-001"
	});

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	const auto& settings = result.value();

	BOOST_CHECK_EQUAL(settings.enabled, true);
	BOOST_CHECK_EQUAL(settings.broker_host, "secure.broker.com");
	BOOST_CHECK_EQUAL(settings.broker_port, 8883);
	BOOST_CHECK_EQUAL(settings.tls.use_tls, true);
	BOOST_CHECK_EQUAL(settings.tls.tls_ca_cert, "/etc/ssl/certs/ca.pem");
	BOOST_CHECK_EQUAL(settings.tls.tls_client_cert, "/etc/ssl/certs/client.pem");
	BOOST_CHECK_EQUAL(settings.tls.tls_client_key, "/etc/ssl/private/client.key");
	BOOST_CHECK_EQUAL(settings.client_id, "pool-controller-001");
	BOOST_CHECK_EQUAL(settings.username, "pool_user");
	BOOST_CHECK_EQUAL(settings.password, "secure_password");
	BOOST_CHECK_EQUAL(settings.topic_prefix, "home/pool");
	BOOST_CHECK_EQUAL(settings.status_publish_interval.count(), 15);
	BOOST_CHECK_EQUAL(settings.statistics_publish_interval.count(), 120);
	BOOST_CHECK_EQUAL(settings.home_assistant_enabled, true);
	BOOST_CHECK_EQUAL(settings.ha_discovery_prefix, "ha_custom");
	BOOST_CHECK_EQUAL(settings.ha_device_id, "pool-controller-001");
}

//-----------------------------------------------------------------------------
// FULL PIPELINE
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_FullPipeline_Defaults)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());

	BOOST_CHECK_EQUAL(result.value().enabled, false);
	BOOST_CHECK_EQUAL(result.value().broker_host, "localhost");
	BOOST_CHECK_EQUAL(result.value().broker_port, 1883);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_FullPipeline_HaDependencyFails)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--home-assistant" });

	// HA requires MQTT - this should fail validation
	BOOST_CHECK_THROW(processor.Validate(vm), Exceptions::Options_MissingDependency);
}

//-----------------------------------------------------------------------------
// PROTOCOL VERSION (admin-selectable 3.1.1- vs 5.0-compatible MQTT)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_DefaultsTo311)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program" });

	BOOST_CHECK_NO_THROW(processor.Validate(vm));

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(result.value().protocol_version == Options::Mqtt::ProtocolVersion::v3_1_1);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_Selects50)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt-protocol-version", "5.0" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(result.value().protocol_version == Options::Mqtt::ProtocolVersion::v5);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_Selects311Explicitly)
{
	Options::Mqtt::OptionsProcessor processor;
	auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt-protocol-version", "3.1.1" });

	auto result = processor.Process(vm);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(result.value().protocol_version == Options::Mqtt::ProtocolVersion::v3_1_1);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_AcceptsAliases)
{
	{
		Options::Mqtt::OptionsProcessor processor;
		auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt-protocol-version", "v5" });
		auto result = processor.Process(vm);
		BOOST_REQUIRE(result.has_value());
		BOOST_CHECK(result.value().protocol_version == Options::Mqtt::ProtocolVersion::v5);
	}
	{
		Options::Mqtt::OptionsProcessor processor;
		auto vm = Test::ParseMqttOptions(processor, { "program", "--mqtt-protocol-version", "311" });
		auto result = processor.Process(vm);
		BOOST_REQUIRE(result.has_value());
		BOOST_CHECK(result.value().protocol_version == Options::Mqtt::ProtocolVersion::v3_1_1);
	}
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_InvalidRejected)
{
	Options::Mqtt::OptionsProcessor processor;
	// Parse-time validation: the validator throws (validation_error derives from
	// boost::program_options::error) during po::store inside ParseMqttOptions.
	BOOST_CHECK_THROW(
		Test::ParseMqttOptions(processor, { "program", "--mqtt-protocol-version", "4.0" }),
		boost::program_options::error);
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_ToStringCanonical)
{
	BOOST_CHECK_EQUAL(std::string{ Options::Mqtt::ToString(Options::Mqtt::ProtocolVersion::v3_1_1) }, "3.1.1");
	BOOST_CHECK_EQUAL(std::string{ Options::Mqtt::ToString(Options::Mqtt::ProtocolVersion::v5) }, "5.0");
}

BOOST_AUTO_TEST_CASE(Test_MqttOptions_ProtocolVersion_ValidatorDirectly)
{
	{
		boost::any val;
		Options::Mqtt::ProtocolVersion target{};
		const std::vector<std::string> values{ "5.0" };
		Options::Mqtt::validate(val, values, &target, 0);
		BOOST_CHECK(boost::any_cast<Options::Mqtt::ProtocolVersion>(val) == Options::Mqtt::ProtocolVersion::v5);
	}
	{
		// Empty value must fail cleanly (no out-of-bounds read), like the other enum validators.
		boost::any val;
		Options::Mqtt::ProtocolVersion target{};
		const std::vector<std::string> values{ std::string{} };
		BOOST_CHECK_THROW(Options::Mqtt::validate(val, values, &target, 0), boost::program_options::validation_error);
	}
}

BOOST_AUTO_TEST_SUITE_END()
