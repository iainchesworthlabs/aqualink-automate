#pragma once

#include <memory>

#include "interfaces/idevice.h"
#include "interfaces/istatuspublisher.h"
#include "devices/jandy_device_types.h"
#include "utility/jandy_slot_connection_manager.h"

namespace AqualinkAutomate::Devices
{

	class JandyDevice : public Interfaces::IDevice, public Interfaces::IStatusPublisher
	{
	public:
		explicit JandyDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id);
		~JandyDevice() override = default;

		const Devices::JandyDeviceType& DeviceId() const override;

	protected:
		Utility::JandySlotConnectionManager m_SlotManager;
	};

}
// namespace AqualinkAutomate::Devices
