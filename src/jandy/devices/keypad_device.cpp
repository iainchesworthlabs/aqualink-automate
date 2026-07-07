#include <format>
#include <functional>

#include <nlohmann/json.hpp>

#include "logging/logging.h"
#include "devices/keypad_device.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	KeypadDevice::KeypadDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(KEYPAD_TIMEOUT_DURATION),
		Capabilities::Screen(KEYPAD_PAGE_LINES),
		Capabilities::Emulated(is_emulated)
	{
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Probe>(std::bind(&KeypadDevice::Slot_Keypad_Probe, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Message>(std::bind(&KeypadDevice::Slot_Keypad_Message, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_MessageLong>(std::bind(&KeypadDevice::Slot_Keypad_MessageLong, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Status>(std::bind(&KeypadDevice::Slot_Keypad_Status, this, std::placeholders::_1), (*device_id)());

		if (!IsEmulated())
		{
			m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Ack>(std::bind(&KeypadDevice::Slot_Keypad_Ack, this, std::placeholders::_1), (*device_id)());
		}
	}
	

	void KeypadDevice::ProcessControllerUpdates()
	{
		// Intentionally empty: the RS Keypad is a passive decoder and has no periodic
		// controller-side state to advance; ACKs are emitted directly from the slot handlers.
	}

	void KeypadDevice::WatchdogTimeoutOccurred()
	{
		// Intentionally empty: loss of communications on the passive RS Keypad requires no
		// device-side state change.
	}

	nlohmann::json KeypadDevice::DescribeDiagnostics() const
	{
		nlohmann::json j;

		j["device_type"] = "Keypad";
		j["device_id"] = std::format("0x{:02x}", DeviceId().Id()());

		j["screen"] = DescribeScreen();

		j["is_emulated"] = IsEmulated();
		j["emulation_suppressed"] = IsEmulationSuppressed();
		j["is_running"] = IsRunning();

		return j;
	}

}
// namespace AqualinkAutomate::Devices
