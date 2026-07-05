#pragma once

#include <format>
#include <string>
#include <string_view>


/// Single source of truth for the per-device MQTT topic layout shared by the
/// publisher (MqttHub) and the Home Assistant discovery layer (HomeAssistantDiscovery).
///
/// Historically these two layers each open-coded their own topic strings, which
/// drifted apart (e.g. the hub tagged auxiliaries "auxillary" while discovery used
/// "aux"). Routing both through TopicScheme guarantees that a discovery component's
/// state_topic always matches the topic the publisher actually writes to.
///
/// Two per-device subtopics exist by design:
///   - DeviceJsonSubtopic   -> a full JSON status blob (consumed by template-based
///                             HA entities, e.g. the chlorinator generating %/boost).
///   - DeviceStateSubtopic  -> a short single-string state (consumed by simple HA
///                             switch/sensor entities).
///
/// All helpers return a subtopic RELATIVE to the configured topic prefix; pass the
/// result through MqttClient::BuildTopic() to obtain the fully-qualified wire topic.
namespace AqualinkAutomate::Mqtt::TopicScheme
{

	/// Logical device category. The string spelling is centralised here so the
	/// publisher's JSON "type" field and the discovery topic/category cannot drift.
	enum class DeviceCategory
	{
		Auxillary,
		Heater,
		Pump,
		Chlorinator
	};

	/// Canonical wire spelling for a device category. Used both as the publisher's
	/// JSON "type" field and as the prefix in a per-device state subtopic.
	[[nodiscard]] constexpr std::string_view CategoryName(DeviceCategory category) noexcept
	{
		using enum DeviceCategory;
		switch (category)
		{
		case Auxillary:   return "aux";
		case Heater:      return "heater";
		case Pump:        return "pump";
		case Chlorinator: return "chlorinator";
		}

		return "unknown";
	}

	/// Subtopic carrying the full JSON status blob for a single device.
	/// e.g. "device/filter_pump".
	[[nodiscard]] inline std::string DeviceJsonSubtopic(std::string_view slug)
	{
		return std::format("device/{}", slug);
	}

	/// Subtopic carrying the short single-string state for a single device, namespaced
	/// by category so two devices with the same label in different categories do not
	/// collide. e.g. "ha/pump_filter_pump".
	[[nodiscard]] inline std::string DeviceStateSubtopic(DeviceCategory category, std::string_view slug)
	{
		return std::format("ha/{}_{}", CategoryName(category), slug);
	}

}
// namespace AqualinkAutomate::Mqtt::TopicScheme
