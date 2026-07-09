#include "devices/device_status.h"
#include "devices/jandy_device.h"

namespace AqualinkAutomate::Devices
{

	JandyDevice::JandyDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id) :
		IDevice(device_id),
		IStatusPublisher(DeviceStatus_Unknown{})
	{
	}


	const Devices::JandyDeviceType& JandyDevice::DeviceId() const
	{
		return dynamic_cast<const Devices::JandyDeviceType&>(IDevice::DeviceId());
	}

}
// namespace AqualinkAutomate::Devices

