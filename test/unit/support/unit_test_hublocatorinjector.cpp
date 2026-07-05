#include <memory>

#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "scheduling/controller_schedule.h"

#include "support/unit_test_hublocatorinjector.h"

namespace AqualinkAutomate::Test
{

	HubLocatorInjector::HubLocatorInjector()
	{
		Register(std::make_shared<Kernel::DataHub>());
		Register(std::make_shared<Kernel::EquipmentHub>());
		Register(std::make_shared<Kernel::PreferencesHub>());
		Register(std::make_shared<Kernel::StatisticsHub>());
		// The controller-schedule sink (the /api/controller/schedules source) that the
		// OneTouch/IAQ devices resolve in their constructors, so a device built by a test
		// fixture can publish its scraped programs somewhere observable. Mirrors
		// aqualink-automate.cpp, where the store is registered ahead of Jandy::Configure.
		Register(std::make_shared<Scheduling::ControllerScheduleStore>());
	}

	HubLocatorInjector::~HubLocatorInjector() = default;

}
// namespace AqualinkAutomate::Test
