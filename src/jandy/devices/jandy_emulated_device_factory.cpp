#include "devices/jandy_emulated_device_factory.h"

#include "devices/iaq_device.h"
#include "devices/jandy_device_types.h"
#include "devices/keypad_device.h"
#include "devices/onetouch_device.h"
#include "devices/pda_device.h"
#include "devices/serial_adapter_device.h"
#include "devices/spaside_remote_device.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	std::unique_ptr<Interfaces::IDevice> MakeEmulatedDevice(JandyEmulatedDeviceTypes type, JandyDeviceId id, Kernel::HubLocator& hub_locator)
	{
		auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceType(id));

		using enum JandyEmulatedDeviceTypes;

		switch (type)
		{
		case OneTouch:
			return std::make_unique<OneTouchDevice>(device_id, hub_locator, true);

		case RS_Keypad:
			return std::make_unique<KeypadDevice>(device_id, hub_locator, true);

		case IAQ:
			return std::make_unique<IAQDevice>(device_id, hub_locator, true);

		case PDA:
			return std::make_unique<PDADevice>(device_id, hub_locator, true);

		case SerialAdapter:
			return std::make_unique<SerialAdapterDevice>(device_id, hub_locator, true);

		case SpasideRemote:
			return std::make_unique<SpasideRemoteDevice>(device_id, hub_locator, true);

		case Unknown:
		default:
			LogWarning(Channel::Devices, "MakeEmulatedDevice: unknown emulated device type; cannot create device");
			return nullptr;
		}
	}

}
// namespace AqualinkAutomate::Devices
