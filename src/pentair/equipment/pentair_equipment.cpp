#include <format>
#include <functional>

#include "devices/pentair_chlorinator_device.h"
#include "devices/pentair_controller_device.h"
#include "devices/pentair_device_id.h"
#include "devices/pentair_vsp_pump_device.h"
#include "equipment/equipment_status.h"
#include "equipment/pentair_equipment.h"
#include "messages/pentair_message_constants.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Equipment
{

	PentairEquipment::PentairEquipment(Kernel::HubLocator& hub_locator) :
		IEquipment(),
		IStatusPublisher(AqualinkAutomate::Equipment::EquipmentStatus_Unknown{}),
		m_HubLocator(hub_locator)
	{
		m_DataHub = m_HubLocator.Find<Kernel::DataHub>();
		m_EquipmentHub = m_HubLocator.Find<Kernel::EquipmentHub>().get();
		m_StatsHub = m_HubLocator.Find<Kernel::StatisticsHub>();

		m_MessageConnections.push_back(
			Messages::PentairPumpMessage_Status::GetSignal()->connect(
				std::bind(&PentairEquipment::IdentifyAndAddPump, this, std::placeholders::_1)));

		m_MessageConnections.push_back(
			Messages::PentairChlorinatorMessage_Status::GetSignal()->connect(
				std::bind(&PentairEquipment::IdentifyAndAddChlorinator, this, std::placeholders::_1)));

		m_MessageConnections.push_back(
			Messages::PentairControllerMessage_Status::GetSignal()->connect(
				std::bind(&PentairEquipment::IdentifyAndAddController, this, std::placeholders::_1)));
	}

	PentairEquipment::~PentairEquipment()
	{
		for (const auto& connection : m_MessageConnections)
		{
			connection.disconnect();
		}
	}

	void PentairEquipment::IdentifyAndAddPump(const Messages::PentairPumpMessage_Status& message)
	{
		// Status frames are broadcast by the pump, so FROM is the pump address.
		// VSP pumps occupy the 0x60-0x6F address block.
		IdentifyAndAdd<Devices::PentairVSPPumpDevice>(
			message.From(),
			"VSP pump device",
			[](uint8_t address) { return (Messages::PUMP_ADDRESS_BASE <= address) && (address <= Messages::PUMP_ADDRESS_LAST); });
	}

	void PentairEquipment::IdentifyAndAddChlorinator(const Messages::PentairChlorinatorMessage_Status& message)
	{
		// The IntelliChlor SWG always reports from the dedicated chlorinator
		// address; reject any other FROM so a malformed/spoofed frame cannot spawn
		// a phantom chlorinator device.
		IdentifyAndAdd<Devices::PentairChlorinatorDevice>(
			message.From(),
			"IntelliChlor device",
			[](uint8_t address) { return address == Messages::CHLORINATOR_ADDRESS; });
	}

	void PentairEquipment::IdentifyAndAddController(const Messages::PentairControllerMessage_Status& message)
	{
		// The controller (IntelliCenter / EasyTouch) broadcasts from the fixed
		// controller/master address; reject any other FROM for the same reason.
		IdentifyAndAdd<Devices::PentairControllerDevice>(
			message.From(),
			"controller device",
			[](uint8_t address) { return address == Messages::CONTROLLER_ADDRESS; });
	}

}
// namespace AqualinkAutomate::Pentair::Equipment
