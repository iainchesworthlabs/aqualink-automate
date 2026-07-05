#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/signals2.hpp>

#include "interfaces/iequipment.h"
#include "interfaces/istatuspublisher.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/statistics_hub.h"
#include "logging/logging.h"
#include "devices/pentair_device_id.h"
#include "messages/chlorinator/pentair_chlorinator_message_status.h"
#include "messages/controller/pentair_controller_message_status.h"
#include "messages/pump/pentair_pump_message_status.h"

namespace AqualinkAutomate::Pentair::Equipment
{

	// Pentair equipment coordinator.  Mirrors JandyEquipment: it subscribes to
	// decoded Pentair messages and, on first sight of a device at a known address
	// range, instantiates the matching device handler and registers it with the
	// EquipmentHub (which then participates in further decoding via its slots).
	class PentairEquipment : public Interfaces::IEquipment, public Interfaces::IStatusPublisher
	{
	public:
		explicit PentairEquipment(Kernel::HubLocator& hub_locator);
		~PentairEquipment() override;

	private:
		void IdentifyAndAddPump(const Messages::PentairPumpMessage_Status& message);
		void IdentifyAndAddChlorinator(const Messages::PentairChlorinatorMessage_Status& message);
		void IdentifyAndAddController(const Messages::PentairControllerMessage_Status& message);

		// Shared device-discovery helper.  Validates the wire FROM address against
		// the supplied predicate (so only addresses inside the device's documented
		// range can ever create a device — an out-of-range / spoofed FROM is
		// dropped rather than spawning an unbounded number of phantom devices),
		// skips creation if a device already exists at that address, and otherwise
		// constructs DEVICE and registers it with the EquipmentHub.
		template<typename DEVICE, typename ADDRESS_PREDICATE>
		void IdentifyAndAdd(uint8_t address, std::string_view human_readable_kind, ADDRESS_PREDICATE&& address_is_valid)
		{
			if (nullptr == m_EquipmentHub)
			{
				return;
			}

			if (!address_is_valid(address))
			{
				return;
			}

			if (Devices::PentairDeviceId candidate_id(address); m_EquipmentHub->DeviceExists(candidate_id))
			{
				return;
			}

			LogInfo(Logging::Channel::Equipment, [human_readable_kind, address] { return std::format("Adding new Pentair {} at address 0x{:02x}", human_readable_kind, address); });

			auto device_id = std::make_shared<Devices::PentairDeviceId>(address);
			m_EquipmentHub->AddDevice(std::make_unique<DEVICE>(device_id, m_HubLocator));
		}

	private:
		std::vector<boost::signals2::connection> m_MessageConnections;

		Kernel::HubLocator& m_HubLocator;
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		// Raw (non-owning) pointer, NOT a shared_ptr: the EquipmentHub OWNS this
		// PentairEquipment (it is held by unique_ptr in the hub), so a shared_ptr
		// back to the hub forms a reference cycle that leaks the hub + all
		// equipment/devices at shutdown. Worse, because this object's static
		// message-signal slots are then never disconnected, a later signal
		// emission invokes them with a dangling HubLocator reference (use-after-
		// free). The hub always outlives the equipment, so a raw pointer is safe.
		Kernel::EquipmentHub* m_EquipmentHub{ nullptr };
		std::shared_ptr<Kernel::StatisticsHub> m_StatsHub{ nullptr };
	};

}
// namespace AqualinkAutomate::Pentair::Equipment
