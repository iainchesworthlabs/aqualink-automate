#include "devices/device_status.h"
#include "devices/pentair_device.h"

namespace AqualinkAutomate::Pentair::Devices
{

	PentairDevice::PentairDevice(const std::shared_ptr<PentairDeviceId>& device_id) :
		IDevice(device_id),
		IStatusPublisher(AqualinkAutomate::Devices::DeviceStatus_Unknown{})
	{
	}

	const PentairDeviceId& PentairDevice::DeviceId() const
	{
		return dynamic_cast<const PentairDeviceId&>(IDevice::DeviceId());
	}

}
// namespace AqualinkAutomate::Pentair::Devices
