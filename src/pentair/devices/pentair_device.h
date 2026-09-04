#pragma once

#include <memory>

#include "interfaces/idevice.h"
#include "interfaces/istatuspublisher.h"
#include "utility/slot_connection_manager.h"
#include "devices/pentair_device_id.h"

namespace AqualinkAutomate::Pentair::Devices
{

	// Base class for Pentair device handlers.  Mirrors the Jandy device base
	// (core IDevice + IStatusPublisher + a slot-connection owner) but uses the
	// shared SlotConnectionManager directly: Pentair messages are filtered by
	// their FROM/DEST address inside each slot handler rather than via the Jandy
	// device-id-filtered slot variant.
	class PentairDevice : public Interfaces::IDevice, public Interfaces::IStatusPublisher
	{
	public:
		explicit PentairDevice(const std::shared_ptr<PentairDeviceId>& device_id);
		~PentairDevice() override = default;

		const PentairDeviceId& DeviceId() const override;

	protected:
		Utility::SlotConnectionManager m_SlotManager;
	};

}
// namespace AqualinkAutomate::Pentair::Devices
