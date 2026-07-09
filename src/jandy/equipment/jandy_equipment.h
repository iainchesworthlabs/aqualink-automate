#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include <boost/signals2.hpp>

#include "interfaces/iequipment.h"
#include "interfaces/istatuspublisher.h"
#include "devices/jandy_device.h"
#include "devices/jandy_device_id.h"
#include "devices/jandy_device_types.h"
#include "generator/jandy_message_generator.h"
#include "messages/jandy_message.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/statistics_hub.h"
#include "protocol/protocol_handler.h"

namespace AqualinkAutomate::Equipment
{

	class JandyEquipment : public Interfaces::IEquipment, public Interfaces::IStatusPublisher
	{
	public:
		explicit JandyEquipment(Kernel::HubLocator& hub_locator, bool decode_to_master = false);
		~JandyEquipment() override;

	private:
		void IdentifyAndAddDevice(const Messages::JandyMessage& message);

		// Connect IdentifyAndAddDevice as a slot of every supplied message type's
		// static signal in one fold expression, replacing the 38 byte-identical
		// push_back(...->GetSignal()->connect(std::bind(...))) lines.  Each MSGS
		// exposes a static GetSignal() (CRTP IMessageSignalRecv<MSGS>); the slot
		// receives a 'const MSGS&' which binds to the 'const JandyMessage&'
		// parameter via the public base.
		template <typename... MSGS>
		void ConnectIdentify()
		{
			(m_MessageConnections.push_back(
				MSGS::GetSignal()->connect(
					[this](const Messages::JandyMessage& message) { IdentifyAndAddDevice(message); })),
				...);
		}

		void DisplayUnknownMessages(const Messages::JandyMessage& message);

		std::vector<boost::signals2::connection> m_MessageConnections;

		// Device classes already reported as unsupported, so the per-message
		// dispatch emits a single Notify per distinct class instead of a Debug
		// line on every message addressed to an unsupported class (rate limiting).
		std::unordered_set<Devices::DeviceClasses> m_ReportedUnsupportedClasses;

		Kernel::HubLocator& m_HubLocator;
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		// Raw (non-owning) pointer, NOT a shared_ptr: the EquipmentHub OWNS this
		// JandyEquipment (held by unique_ptr in the hub), so a shared_ptr back to
		// the hub forms a reference cycle that leaks the hub + all equipment and
		// devices at shutdown, and leaves this object's static message-signal
		// slots connected with a dangling HubLocator reference (use-after-free on
		// a later emission). The hub always outlives the equipment.
		Kernel::EquipmentHub* m_EquipmentHub{ nullptr };
		std::shared_ptr<Kernel::StatisticsHub> m_StatsHub{ nullptr };

		// --decode-to-master diagnostic: when true, frames addressed to the master (0x00)
		// are decoded and logged (observe-only). Off in normal operation.
		bool m_DecodeToMaster{ false };
	};

}
// namespace AqualinkAutomate::Equipment
