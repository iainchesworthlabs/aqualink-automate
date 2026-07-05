#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "devices/capabilities/restartable.h"
#include "devices/pentair_device.h"
#include "devices/pentair_device_id.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "messages/controller/pentair_controller_message_status.h"

namespace AqualinkAutomate::Pentair::Devices
{

	// Handler for a Pentair IntelliCenter / EasyTouch controller (address 0x10).
	//
	// Decodes the controller's periodic status broadcast and pushes the system
	// state into the DataHub: water/air temperatures (reported in Fahrenheit),
	// circulation mode (pool vs spa) from the active circuit, and pool/spa heater
	// state.  Keeps a Restartable watchdog alive while the controller reports.
	class PentairControllerDevice : public PentairDevice, public AqualinkAutomate::Devices::Capabilities::Restartable
	{
		inline static const std::chrono::seconds CONTROLLER_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		PentairControllerDevice(const std::shared_ptr<PentairDeviceId>& device_id, Kernel::HubLocator& hub_locator);
		~PentairControllerDevice() override = default;

		bool PoolHeaterOn() const;
		bool SpaHeaterOn() const;

	private:
		void WatchdogTimeoutOccurred() override;

		void Slot_Controller_Status(const Messages::PentairControllerMessage_Status& msg);

		void PushStatusToDataHub(const Messages::PentairControllerMessage_Status& msg);


		uint8_t m_Address;
		bool m_PoolHeaterOn{ false };
		bool m_SpaHeaterOn{ false };
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
	};

}
// namespace AqualinkAutomate::Pentair::Devices
