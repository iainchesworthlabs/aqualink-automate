#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/restartable.h"
#include "messages/heater/heater_message_request.h"
#include "messages/heater/heater_message_status.h"

namespace AqualinkAutomate::Devices
{

	class HeaterDevice : public JandyDevice, public Capabilities::Restartable
	{
		inline static const std::chrono::seconds HEATER_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		using TimestampedMode = std::pair<Messages::HeaterOperatingModes, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedTemp = std::pair<uint8_t, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedState = std::pair<Messages::HeaterStates, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedError = std::pair<Messages::HeaterErrors, std::chrono::time_point<std::chrono::system_clock>>;

	public:
		explicit HeaterDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id);
		~HeaterDevice() override = default;

	private:
		void WatchdogTimeoutOccurred() override;

	public:
		TimestampedMode OperatingMode() const;
		TimestampedTemp PoolSetpoint() const;
		TimestampedTemp SpaSetpoint() const;
		TimestampedTemp WaterTemperature() const;
		TimestampedState HeaterState() const;
		TimestampedError ErrorCode() const;

	private:
		TimestampedMode m_OperatingMode;
		TimestampedTemp m_PoolSetpoint;
		TimestampedTemp m_SpaSetpoint;
		TimestampedTemp m_WaterTemperature;
		TimestampedState m_HeaterState;
		TimestampedError m_ErrorCode;

	private:
		void Slot_Heater_Request(const Messages::HeaterMessage_Request& msg);
		void Slot_Heater_Status(const Messages::HeaterMessage_Status& msg);
	};

}
// namespace AqualinkAutomate::Devices
