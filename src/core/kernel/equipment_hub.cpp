#include <format>
#include <memory>
#include <source_location>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include "kernel/equipment_hub.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::Kernel
{

	bool EquipmentHub::EquipmentExists(std::unique_ptr<Interfaces::IEquipment> const& equipment) const
	{
		if (!equipment)
		{
			return false;
		}

		// Key on the *runtime pointee* type (typeid(*equipment)), not the
		// static IEquipment type, so each concrete subclass is distinct.
		// Bind the pointee to a named reference first: clang's
		// -Wpotentially-evaluated-expression (an error under -Werror) fires when
		// typeid's operand is a side-effecting expression (operator* is a call);
		// a named glvalue avoids it while preserving the runtime-type lookup.
		const auto& equipment_ref = *equipment;
		const auto equipment_id = std::type_index(typeid(equipment_ref));
		return m_ActiveEquipment.contains(equipment_id);
	}

	bool EquipmentHub::AddEquipment(std::unique_ptr<Interfaces::IEquipment> equipment_to_add)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("EquipmentHub::AddEquipment", std::source_location::current());

		if (!equipment_to_add)
		{
			LogWarning(Channel::Devices, "Cannot register equipment with equipment hub; equipment object was invalid");
		}
		else if (EquipmentExists(equipment_to_add))
		{
			LogDebug(Channel::Devices, "Failed to register equipment with equipment hub; equipment id already registered");
		}
		// The null check above guarantees the dereference here is safe, so the
		// runtime type used as the key matches the one EquipmentExists tested.
		// (Bind the pointee to a named reference first to avoid clang's
		// -Wpotentially-evaluated-expression on a side-effecting typeid operand.)
		else
		{
			const auto& equipment_ref = *equipment_to_add;
			const auto equipment_id = std::type_index(typeid(equipment_ref));
			if (!m_ActiveEquipment.emplace(equipment_id, std::move(equipment_to_add)).second)
			{
				LogDebug(Channel::Devices, "Failed to add equipment to equipment hub; internal error while adding equipment object");
			}
			else
			{
				return true;
			}
		}

		return false;
	}

	bool EquipmentHub::DeviceExists(const Interfaces::IDeviceIdentifier& device_id) const
	{
		// Limit the existence scan to devices whose identifier shares the same
		// runtime type as the one being queried; IDeviceIdentifier exposes only
		// Equals() so a within-bucket comparison is still required.
		const auto identifier_type = std::type_index(typeid(device_id));
		const auto [bucket_begin, bucket_end] = m_ActiveDevices.equal_range(identifier_type);

		for (auto it = bucket_begin; it != bucket_end; ++it)
		{
			if (device_id == it->second->DeviceId())
			{
				return true;
			}
		}

		return false;
	}

	bool EquipmentHub::DeviceExists(std::unique_ptr<Interfaces::IDevice> const& device) const
	{
		if (!device)
		{
			return false;
		}

		// Delegate to the IDeviceIdentifier overload so the bucketed lookup
		// lives in exactly one place.
		return DeviceExists(device->DeviceId());
	}

	bool EquipmentHub::AddDevice(std::unique_ptr<Interfaces::IDevice> device_to_add)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("EquipmentHub::AddDevice", std::source_location::current());

		bool added_device_successfully = false;

		if (!device_to_add)
		{
			LogWarning(Channel::Devices, "Cannot register device with equipment hub; device object was invalid");
		}
		else if (DeviceExists(device_to_add))
		{
			LogDebug(Channel::Devices, "Failed to register device with equipment hub; device id was already registered");
		}
		else
		{
			// Bind the identifier to a named reference first to avoid clang's
			// -Wpotentially-evaluated-expression on a side-effecting typeid operand.
			const auto& device_id_ref = device_to_add->DeviceId();
			const auto identifier_type = std::type_index(typeid(device_id_ref));
			m_ActiveDevices.emplace(identifier_type, std::move(device_to_add));

			LogTrace(Channel::Devices, [size = m_ActiveDevices.size()] { return std::format("Registered device with equipment hub (active devices count = {})", size); });

			added_device_successfully = true;
		}

		// Gauge the live device count on the profiler timeline (cheap; updated only
		// on registration, so it tracks discovery progress without per-frame cost).
		Factory::ProfilerFactory::Instance().Get()->PlotValue("Active Devices", static_cast<int64_t>(m_ActiveDevices.size()));

		return added_device_successfully;
	}

}
// namespace AqualinkAutomate::Equipment
