#include <cctype>
#include <format>
#include <string>

#include <boost/program_options/errors.hpp>
#include <boost/program_options/value_semantic.hpp>

#include "logging/logging.h"
#include "options/options_mqtt_options.h"
#include "options/helpers/option_dependency_helper.h"
#include "utility/get_terminal_column_width.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options::Mqtt
{

	// boost::program_options validator for the MQTT protocol-version option.
	// In the validated type's namespace so it is found by ADL. Accepts friendly
	// admin spellings ("3.1.1"/"5.0" + aliases) rather than the enumerator names.
	void validate(boost::any& v, const std::vector<std::string>& values, ProtocolVersion*, int)
	{
		boost::program_options::validators::check_first_occurrence(v);
		const std::string& token = boost::program_options::validators::get_single_string(values);

		// Normalise: drop whitespace, lower-case, strip a leading 'v' so "3.1.1",
		// "v3.1.1", "5", "v5", "5.0" all resolve naturally. No indexing of the raw
		// string, so an empty value fails cleanly via validation_error below.
		std::string norm;
		norm.reserve(token.size());
		for (char c : token)
		{
			if (!std::isspace(static_cast<unsigned char>(c)))
			{
				norm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
		}
		if (!norm.empty() && norm.front() == 'v')
		{
			norm.erase(norm.begin());
		}

		if ((norm == "3.1.1") || (norm == "3.1") || (norm == "311") || (norm == "3"))
		{
			v = boost::any(ProtocolVersion::v3_1_1);
		}
		else if ((norm == "5") || (norm == "5.0"))
		{
			v = boost::any(ProtocolVersion::v5);
		}
		else
		{
			LogDebug(Channel::Options, std::format("Invalid MQTT protocol version -> provided string was: '{}'", token));
			throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
		}
	}

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		boost::program_options::options_description options(SettingsType::AreaName(), Utility::get_terminal_column_width());

		LogDebug(Channel::Options, std::format("Adding {} options from the {} set", MqttOptionsCollection.size(), SettingsType::AreaName()));
		for (auto& option : MqttOptionsCollection)
		{
			options.add((*option)());
		}

		return options;
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& vm) const
	{
		// TLS certificate dependencies
		Helper_ValidateOptionDependencies(vm, OPTION_TLS_CLIENT_CERT, OPTION_TLS_CLIENT_KEY);
		Helper_ValidateOptionDependencies(vm, OPTION_TLS_CLIENT_KEY, OPTION_TLS_CLIENT_CERT);

		// Home Assistant requires MQTT to be enabled
		Helper_ValidateOptionDependencies(vm, OPTION_HOME_ASSISTANT, OPTION_ENABLE);

		// HA device ID requires Home Assistant to be enabled
		Helper_ValidateOptionDependencies(vm, OPTION_HA_DEVICE_ID, OPTION_HOME_ASSISTANT);
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		// Enable/disable
		if (OPTION_ENABLE->IsPresent(vm))
		{
			settings.enabled = OPTION_ENABLE->As<bool>(vm);
		}

		// Protocol version (admin selects 3.1.1- or 5.0-compatible MQTT)
		if (OPTION_PROTOCOL_VERSION->IsPresent(vm))
		{
			settings.protocol_version = OPTION_PROTOCOL_VERSION->As<ProtocolVersion>(vm);
		}

		// Broker connection
		if (OPTION_BROKER_HOST->IsPresent(vm))
		{
			settings.broker_host = OPTION_BROKER_HOST->As<std::string>(vm);
		}

		if (OPTION_BROKER_PORT->IsPresent(vm))
		{
			settings.broker_port = OPTION_BROKER_PORT->As<uint16_t>(vm);
		}

		if (OPTION_USE_TLS->IsPresent(vm))
		{
			settings.tls.use_tls = OPTION_USE_TLS->As<bool>(vm);

			// If TLS is enabled and port wasn't explicitly set, use default TLS port
			if (settings.tls.use_tls && !OPTION_BROKER_PORT->IsPresentAndNotJustDefaulted(vm))
			{
				settings.broker_port = 8883;
			}
		}

		if (OPTION_TLS_CA_CERT->IsPresent(vm))
		{
			settings.tls.tls_ca_cert = OPTION_TLS_CA_CERT->As<std::string>(vm);
		}

		if (OPTION_TLS_CLIENT_CERT->IsPresent(vm))
		{
			settings.tls.tls_client_cert = OPTION_TLS_CLIENT_CERT->As<std::string>(vm);
		}

		if (OPTION_TLS_CLIENT_KEY->IsPresent(vm))
		{
			settings.tls.tls_client_key = OPTION_TLS_CLIENT_KEY->As<std::string>(vm);
		}

		if (OPTION_TLS_SKIP_VERIFY->IsPresent(vm))
		{
			settings.tls.tls_skip_verify = OPTION_TLS_SKIP_VERIFY->As<bool>(vm);
		}

		// Authentication
		if (OPTION_CLIENT_ID->IsPresent(vm))
		{
			settings.client_id = OPTION_CLIENT_ID->As<std::string>(vm);
		}

		if (OPTION_USERNAME->IsPresent(vm))
		{
			settings.username = OPTION_USERNAME->As<std::string>(vm);
		}

		if (OPTION_PASSWORD->IsPresent(vm))
		{
			settings.password = OPTION_PASSWORD->As<std::string>(vm);
		}

		// Topic configuration
		if (OPTION_TOPIC_PREFIX->IsPresent(vm))
		{
			settings.topic_prefix = OPTION_TOPIC_PREFIX->As<std::string>(vm);
		}

		// Publishing intervals
		if (OPTION_STATUS_INTERVAL->IsPresent(vm))
		{
			settings.status_publish_interval = std::chrono::seconds(OPTION_STATUS_INTERVAL->As<uint32_t>(vm));
		}

		if (OPTION_STATS_INTERVAL->IsPresent(vm))
		{
			settings.statistics_publish_interval = std::chrono::seconds(OPTION_STATS_INTERVAL->As<uint32_t>(vm));
		}

		if (OPTION_PUBLISH_ON_CHANGE->IsPresent(vm))
		{
			settings.publish_on_change = OPTION_PUBLISH_ON_CHANGE->As<bool>(vm);
		}

		// Home Assistant
		if (OPTION_HOME_ASSISTANT->IsPresent(vm))
		{
			settings.home_assistant_enabled = OPTION_HOME_ASSISTANT->As<bool>(vm);
		}

		if (OPTION_HA_DISCOVERY_PREFIX->IsPresent(vm))
		{
			settings.ha_discovery_prefix = OPTION_HA_DISCOVERY_PREFIX->As<std::string>(vm);
		}

		if (OPTION_HA_DEVICE_ID->IsPresent(vm))
		{
			settings.ha_device_id = OPTION_HA_DEVICE_ID->As<std::string>(vm);
		}

		// Derive ha_device_id from topic_prefix if not explicitly set
		if (settings.ha_device_id.empty())
		{
			std::string slug;
			slug.reserve(settings.topic_prefix.size());
			for (char c : settings.topic_prefix)
			{
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
				{
					slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				else if (c == ' ' || c == '-' || c == '.' || c == '/')
				{
					if (!slug.empty() && slug.back() != '_')
					{
						slug += '_';
					}
				}
			}
			if (!slug.empty() && slug.back() == '_')
			{
				slug.pop_back();
			}
			settings.ha_device_id = std::format("aqualink_{}", slug);
		}

		LogInfo(Channel::Options, std::format("MQTT settings: enabled={}, broker={}:{}, tls={}, protocol={}, ha={}",
			settings.enabled, settings.broker_host, settings.broker_port, settings.tls.use_tls, ToString(settings.protocol_version), settings.home_assistant_enabled));

		return settings;
	}

}
// namespace AqualinkAutomate::Options::Mqtt
