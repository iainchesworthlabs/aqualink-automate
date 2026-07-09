#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/restartable.h"
#include "messages/chemlink/chemlink_message_response.h"

namespace AqualinkAutomate::Devices
{

	class ChemlinkDevice : public JandyDevice, public Capabilities::Restartable
	{
		inline static const std::chrono::seconds CHEMLINK_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		using TimestampedORP = std::pair<uint16_t, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedPH = std::pair<double, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedFeederState = std::pair<bool, std::chrono::time_point<std::chrono::system_clock>>;

	public:
		explicit ChemlinkDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id);
		~ChemlinkDevice() override = default;

	private:
		void WatchdogTimeoutOccurred() override;

	public:
		TimestampedORP ORPMillivolts() const;
		TimestampedPH PHValue() const;
		TimestampedFeederState PHFeederRunning() const;
		TimestampedFeederState ORPFeederRunning() const;

	private:
		TimestampedORP m_ORPMillivolts;
		TimestampedPH m_PHValue;
		TimestampedFeederState m_PHFeederRunning;
		TimestampedFeederState m_ORPFeederRunning;

	private:
		void Slot_Chemlink_Response(const Messages::ChemlinkMessage_Response& msg);
	};

}
// namespace AqualinkAutomate::Devices
