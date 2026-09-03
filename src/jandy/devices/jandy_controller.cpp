#include "devices/jandy_controller.h"

#include "auxillaries/jandy_auxillary_presence_override.h"

namespace AqualinkAutomate::Devices
{

	JandyController::JandyController(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator) :
		JandyDevice(device_id)
	{
		m_DataHub = hub_locator.Find<Kernel::DataHub>();
		m_PreferencesHub = hub_locator.TryFind<Kernel::PreferencesHub>();

		// Restore any aux presence override persisted from a previous run. Runs once per
		// controller construction (harmless if more than one controller does it -- the
		// reconciliation is idempotent), which is what makes a Present override actually
		// reappear after a restart rather than sitting inert in PreferencesHub until the
		// operator re-touches it via the UI.
		if (nullptr != m_PreferencesHub)
		{
			Auxillaries::ApplyPresenceOverrides(m_DataHub->Devices, m_PreferencesHub->AuxPresenceOverrides);
		}
	}


}
// namespace AqualinkAutomate::Devices
