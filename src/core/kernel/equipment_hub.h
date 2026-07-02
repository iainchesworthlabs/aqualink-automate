#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include <boost/signals2.hpp>

#include "interfaces/idevice.h"
#include "interfaces/ideviceidentifier.h"
#include "interfaces/iequipment.h"
#include "interfaces/ihub.h"
#include "kernel/hub_events/equipment_hub_system_event.h"
#include "kernel/hub_events/equipment_hub_system_event_status_change.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{

	struct EquipmentWithConnectedDevices
	{
		std::shared_ptr<Interfaces::IEquipment> Equipment;
		std::vector<std::unique_ptr<Interfaces::IDevice>> Devices;
	};

	class EquipmentHub : public Interfaces::IHub
	{
	public:
		EquipmentHub() = default;
		~EquipmentHub() override = default;

		//---------------------------------------------------------------------
		// UPDATES / NOTIFICATIONS / EVENTS
		//---------------------------------------------------------------------

	public:
		mutable boost::signals2::signal<void(std::shared_ptr<Kernel::EquipmentHub_SystemEvent>)> EquipmentStatusChangeSignal;

		// Alerting (WS3): emitted by AlertMonitor on each fault-condition edge so
		// the WebSocket layer can broadcast it to UI clients.
		// Args: condition key, raised(true)/cleared(false), unix-seconds timestamp,
		// human-readable ENGLISH detail, structured params behind the detail
		// (empty object when none; the UI builds its translated text from
		// condition + params, see docs/i18n.md).
		mutable boost::signals2::signal<void(const std::string&, bool, std::int64_t, const std::string&, const nlohmann::json&)> AlertTransitionSignal;

		//---------------------------------------------------------------------
		// ACTIVE EQUIPMENT
		//---------------------------------------------------------------------

	public:
		bool EquipmentExists(std::unique_ptr<Interfaces::IEquipment> const& device) const;
		bool AddEquipment(std::unique_ptr<Interfaces::IEquipment> device);

		bool DeviceExists(const Interfaces::IDeviceIdentifier& device_id) const;
		bool DeviceExists(std::unique_ptr<Interfaces::IDevice> const& device) const;
		bool AddDevice(std::unique_ptr<Interfaces::IDevice> device);

		// FindDevice returns the first registered device satisfying the
		// predicate, or nullptr.  The predicate is taken by forwarding
		// reference so it can be a lambda passed without the per-call
		// std::function heap allocation the previous std::function-by-value
		// signature incurred on the command-dispatch path.
		template<typename Predicate>
		Interfaces::IDevice* FindDevice(Predicate&& predicate) const
		{
			for (const auto& [identifier_type, device] : m_ActiveDevices)
			{
				if (predicate(*device))
				{
					return device.get();
				}
			}

			return nullptr;
		}

		// NOTE: The equipment hub is intentionally unsynchronised. Device
		// registration (from the protocol task) and iteration (from the
		// HTTP/diagnostics handlers) both run on the single application thread
		// driven by the main poll() loop, so no locking is required. If a
		// multi-threaded execution model is ever reintroduced, this container
		// must be guarded before concurrent iteration/mutation.
		template<typename Func>
		void ForEachDevice(Func&& func) const
		{
			for (const auto& [identifier_type, device] : m_ActiveDevices)
			{
				func(*device);
			}
		}

	private:
		// Equipment is keyed by the *runtime pointee* type so that distinct
		// concrete IEquipment subclasses (e.g. JandyEquipment and
		// PentairEquipment) each register exactly once.  Keying by the static
		// IEquipment type (the historical bug) collapsed every subclass onto a
		// single slot, silently dropping the second protocol's equipment.
		std::unordered_map<std::type_index, std::unique_ptr<Interfaces::IEquipment>> m_ActiveEquipment;

		// Devices are bucketed by the runtime type of their IDeviceIdentifier
		// (typeid(device->DeviceId())).  This bounds the per-id existence scan
		// to devices that share an identifier type rather than scanning every
		// device in the system.  A perfect O(1) by-id map is not possible at
		// this layer because IDeviceIdentifier (src/core/interfaces) exposes
		// only Equals() with no hash or stable scalar key; promoting this to a
		// full O(1) lookup would require adding a hash hook to that interface,
		// which is outside this change's scope.
		std::unordered_multimap<std::type_index, std::unique_ptr<Interfaces::IDevice>> m_ActiveDevices;

	};

}
// namespace AqualinkAutomate::Equipment
