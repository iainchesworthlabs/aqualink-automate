#pragma once

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"

namespace AqualinkAutomate::Devices
{

	class JandyController : public JandyDevice
	{
	public:
		JandyController(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator);
		~JandyController() override = default;

	protected:
		virtual void ProcessControllerUpdates() = 0;

	protected:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };

		// TryFind (not Find): a minimal test HubLocator may not register PreferencesHub, and the
		// aux presence-override it carries is an optional operator setting, not a hard dependency
		// -- callers must null-check before use.
		std::shared_ptr<Kernel::PreferencesHub> m_PreferencesHub{ nullptr };
	};

}
// namespace AqualinkAutomate::Devices
