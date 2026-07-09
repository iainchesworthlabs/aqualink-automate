#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/restartable.h"
#include "messages/epump/epump_message_status.h"
#include "messages/epump/epump_message_rpm.h"
#include "messages/epump/epump_message_watts.h"

namespace AqualinkAutomate::Devices
{

	class EPumpDevice : public JandyDevice, public Capabilities::Restartable
	{
		inline static const std::chrono::seconds EPUMP_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		using TimestampedRPM = std::pair<uint16_t, std::chrono::time_point<std::chrono::system_clock>>;
		using TimestampedWatts = std::pair<uint16_t, std::chrono::time_point<std::chrono::system_clock>>;

	public:
		explicit EPumpDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id);
		~EPumpDevice() override = default;

	private:
		void WatchdogTimeoutOccurred() override;

	public:
		TimestampedRPM ReportedRPM() const;
		TimestampedWatts ReportedWatts() const;

	private:
		TimestampedRPM m_RPM;
		TimestampedWatts m_Watts;

	private:
		void Slot_EPump_Status(const Messages::EPumpMessage_Status& msg);
		void Slot_EPump_RPM(const Messages::EPumpMessage_RPM& msg);
		void Slot_EPump_Watts(const Messages::EPumpMessage_Watts& msg);
	};

}
// namespace AqualinkAutomate::Devices
