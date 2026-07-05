#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/restartable.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "messages/light/light_message_status.h"

namespace AqualinkAutomate::Devices
{

	// LightsDevice handles a Jandy colored-light controller (device ids
	// 0xF0-0xF4).  Following the simple-device pattern (cf. AquariteDevice) it
	// stores the most-recently decoded light state on the device object and
	// surfaces it into the DataHub as an auxillary device (AuxillaryTypes::Light)
	// so the diagnostics / API / MQTT layers can observe it.
	class LightsDevice : public JandyDevice, public Capabilities::Restartable
	{
		inline static const std::chrono::seconds LIGHTS_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		using TimestampedState = std::pair<Messages::LightStates, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedMode = std::pair<uint8_t, std::chrono::time_point<std::chrono::system_clock>>;

	public:
		LightsDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator);
		~LightsDevice() override = default;

	private:
		void WatchdogTimeoutOccurred() override;

	public:
		TimestampedState LightState() const;
		TimestampedMode LightMode() const;
		bool IsOn() const;

	private:
		TimestampedState m_LightState{ Messages::LightStates::Unknown, std::chrono::system_clock::now() };
		TimestampedMode m_LightMode{ static_cast<uint8_t>(0), std::chrono::system_clock::now() };
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };

	private:
		void Slot_Light_Status(const Messages::LightMessage_Status& msg);

		void EnsureLightDeviceExists() const;
		void PushStateToDataHub(const Messages::LightMessage_Status& msg);
	};

}
// namespace AqualinkAutomate::Devices
