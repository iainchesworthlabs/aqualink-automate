#pragma once

#include <chrono>
#include <cstdint>

#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/scrapeable.h"
#include "devices/capabilities/screen.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_message.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_status.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::Devices
{

	class KeypadDevice : public JandyController, public Capabilities::Restartable, public Capabilities::Screen, public Capabilities::Emulated, public Capabilities::Describable
	{
		inline static const uint8_t KEYPAD_PAGE_LINES{ 3 };
		inline static const std::chrono::seconds KEYPAD_TIMEOUT_DURATION{ std::chrono::seconds(30) };

		enum class KeyCommands : uint8_t
		{
			NoKeyCommand = 0x00,
		};

	public:
		KeypadDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~KeypadDevice() override = default;

		nlohmann::json DescribeDiagnostics() const override;

	private:
		void ProcessControllerUpdates() override;

		void WatchdogTimeoutOccurred() override;

		void Slot_Keypad_Ack(const Messages::JandyMessage_Ack& msg);
		void Slot_Keypad_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_Keypad_Message(const Messages::JandyMessage_Message& msg);
		void Slot_Keypad_MessageLong(const Messages::JandyMessage_MessageLong& msg);
		void Slot_Keypad_Status(const Messages::JandyMessage_Status& msg);
	};

}
// namespace AqualinkAutomate::Devices
