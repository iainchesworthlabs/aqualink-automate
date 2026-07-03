#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Alerting
{

	//=========================================================================
	// Alert conditions (WS3 fault-detection v1).
	//
	// Each condition is a latched boolean: it is "raised" while its fault is
	// active and "cleared" otherwise.  The keys are STABLE strings — they are
	// used verbatim as MQTT/JSON state field names, Home Assistant unique-id
	// suffixes, and the `condition` field of the WebSocket AlertTransition
	// payload — so do not rename them without migrating consumers.
	//=========================================================================
	namespace ConditionKeys
	{
		inline constexpr std::string_view ChlorinatorFault{ "chlorinator_fault" };
		inline constexpr std::string_view ChlorinatorWarning{ "chlorinator_warning" };
		inline constexpr std::string_view SaltLow{ "salt_low" };
		inline constexpr std::string_view ServiceMode{ "service_mode" };
		inline constexpr std::string_view SerialCommsLoss{ "serial_comms_loss" };
		inline constexpr std::string_view TemperatureStale{ "temperature_stale" };
	}
	// namespace ConditionKeys

	struct AlertConditionInfo
	{
		std::string_view key;            // stable identifier (see ConditionKeys)
		std::string_view friendly_name;  // Home Assistant entity display name
	};

	// The v1 condition catalogue.  Home Assistant discovery iterates this to
	// register one `binary_sensor` (device_class: problem) per condition.
	//
	// NOTE: device_lost_comms (listed in the WS3 plan) is intentionally NOT in
	// v1: the watchdog path (AquariteDevice::WatchdogTimeoutOccurred) mutates
	// DataHub traits without emitting a per-device status event, so a generic
	// "device lost comms" cannot be detected cleanly without new hub plumbing.
	// It can be added as a fifth entry once such a signal exists.
	inline constexpr std::array<AlertConditionInfo, 6> AlertConditions
	{ {
		{ ConditionKeys::ChlorinatorFault,   "Chlorinator Fault" },
		{ ConditionKeys::ChlorinatorWarning, "Chlorinator Warning" },
		{ ConditionKeys::SaltLow,            "Salt Low" },
		{ ConditionKeys::ServiceMode,        "Service Mode" },
		{ ConditionKeys::SerialCommsLoss,    "Serial Comms Loss" },
		{ ConditionKeys::TemperatureStale,   "Temperature Stale" },
	} };

	// Subtopic (appended to the MQTT topic prefix via MqttClient::BuildTopic)
	// carrying the consolidated alert state document that the HA binary_sensors
	// read.  Both the publisher (AlertMonitor's MQTT sink) and the discovery
	// layer derive their topic from this constant so they always match.
	inline constexpr std::string_view AlertStateSubtopic{ "alert/state" };

	// A single raised/cleared transition for one condition.
	struct AlertTransition
	{
		std::string condition;     // one of ConditionKeys
		bool raised{ false };      // true = raised, false = cleared
		std::int64_t ts{ 0 };      // unix seconds (UTC)
		std::string detail;        // human-readable ENGLISH description (stable contract)
		// Structured values behind the detail text (e.g. {"salt_ppm": 2400,
		// "threshold_ppm": 2700} or {"health": "Warning_LowSalt"}), so consumers
		// — the web UI's translated alerts, webhook automations — can act on the
		// data without parsing prose. Empty object when a transition carries none.
		nlohmann::json params = nlohmann::json::object();
	};

}
// namespace AqualinkAutomate::Alerting
