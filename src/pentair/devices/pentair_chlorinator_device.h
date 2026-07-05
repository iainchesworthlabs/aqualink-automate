#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "devices/capabilities/restartable.h"
#include "devices/pentair_device.h"
#include "devices/pentair_device_id.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "messages/chlorinator/pentair_chlorinator_message_status.h"

namespace AqualinkAutomate::Pentair::Devices
{

	// Handler for a Pentair IntelliChlor salt-water generator (address 0x50).
	//
	// Decodes the chlorinator's status (salt PPM, output %, fault flags), keeps a
	// watchdog alive, surfaces a Chlorinator auxillary in the DataHub (mirroring
	// the Jandy AquaRite device), and can emit a set-output command.
	class PentairChlorinatorDevice : public PentairDevice, public AqualinkAutomate::Devices::Capabilities::Restartable
	{
		inline static const std::chrono::seconds CHLORINATOR_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		PentairChlorinatorDevice(const std::shared_ptr<PentairDeviceId>& device_id, Kernel::HubLocator& hub_locator);
		~PentairChlorinatorDevice() override = default;

		uint16_t ReportedSaltPPM() const;
		uint8_t ReportedOutputPercent() const;

		// Emit a set-output command (controller -> this chlorinator).
		void SetOutput(uint8_t output_percent) const;

	private:
		void WatchdogTimeoutOccurred() override;

		void Slot_Chlorinator_Status(const Messages::PentairChlorinatorMessage_Status& msg);

		void EnsureChlorinatorDeviceExists();
		void PushStatusToDataHub(const Messages::PentairChlorinatorMessage_Status& msg);

	private:
		uint8_t m_Address;
		uint16_t m_SaltPPM{ 0 };
		uint8_t m_OutputPercent{ 0 };
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
	};

}
// namespace AqualinkAutomate::Pentair::Devices
