#include <format>
#include <functional>
#include <source_location>
#include <string>

#include <magic_enum/magic_enum.hpp>

#include "devices/device_status.h"
#include "devices/lights_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{
	namespace
	{
		std::string MakeLightLabel(uint8_t device_id)
		{
			return std::format("Light 0x{:02x}", device_id);
		}

		Kernel::AuxillaryStatuses ConvertToAuxillaryStatus(Messages::LightStates state)
		{
			switch (state)
			{
			case Messages::LightStates::On:  return Kernel::AuxillaryStatuses::On;
			case Messages::LightStates::Off: return Kernel::AuxillaryStatuses::Off;
			default:                         return Kernel::AuxillaryStatuses::Unknown;
			}
		}
	}
	// namespace

	LightsDevice::LightsDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator) :
		JandyDevice(device_id),
		Capabilities::Restartable(LIGHTS_TIMEOUT_DURATION)
	{
		m_DataHub = hub_locator.Find<Kernel::DataHub>();

		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::LightMessage_Status>([this](auto&& PH1) { Slot_Light_Status(std::forward<decltype(PH1)>(PH1)); }, (*device_id)());
	}

	void LightsDevice::WatchdogTimeoutOccurred()
	{
		LogWarning(Channel::Devices, [this]() { return std::format("Lights (0x{:02x}): Watchdog timeout occurred - marking device as having lost communications.", DeviceId().Id()()); });

		Status(Devices::DeviceStatus_LostComms{});

		// Reflect the loss of communications in the DataHub auxillary trait so the
		// API / MQTT layers observe the light as no-longer-known rather than stuck On.
		if (m_DataHub)
		{
			using namespace Kernel::AuxillaryTraitsTypes;

			const auto label = MakeLightLabel(DeviceId().Id()());
			if (auto lights = m_DataHub->Devices.FindByLabel(label); !lights.empty())
			{
				lights.front()->AuxillaryTraits.Set(AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Unknown);
			}
		}
	}

	LightsDevice::TimestampedState LightsDevice::LightState() const
	{
		return m_LightState;
	}

	LightsDevice::TimestampedMode LightsDevice::LightMode() const
	{
		return m_LightMode;
	}

	bool LightsDevice::IsOn() const
	{
		return Messages::LightStates::On == m_LightState.first;
	}

	void LightsDevice::Slot_Light_Status(const Messages::LightMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("LightsDevice::Slot_Light_Status", std::source_location::current());

		LogDebug(Channel::Devices, [&msg]() { return std::format("Lights device received a LightMessage_Status signal. State: {}, Mode: {}", magic_enum::enum_name(msg.State()), msg.LightMode()); });

		m_LightState = std::make_pair(msg.State(), std::chrono::system_clock::now());
		m_LightMode = std::make_pair(msg.LightMode(), std::chrono::system_clock::now());

		PushStateToDataHub(msg);

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void LightsDevice::EnsureLightDeviceExists()
	{
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		const auto label = MakeLightLabel(DeviceId().Id()());
		if (!m_DataHub->Devices.FindByLabel(label).empty())
		{
			return;
		}

		LogInfo(Channel::Devices, [&label]() { return std::format("LightsDevice: Creating light device '{}' from Jandy wire data", label); });

		auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
		ptr->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Light);
		ptr->AuxillaryTraits.Set(LabelTrait{}, label);
		ptr->AuxillaryTraits.Set(AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Unknown);
		ptr->AuxillaryTraits.Set(ColourTrait{}, static_cast<uint8_t>(0));
		m_DataHub->Devices.Add(std::move(ptr));
	}

	void LightsDevice::PushStateToDataHub(const Messages::LightMessage_Status& msg)
	{
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		EnsureLightDeviceExists();

		const auto label = MakeLightLabel(DeviceId().Id()());
		auto lights = m_DataHub->Devices.FindByLabel(label);
		if (lights.empty())
		{
			return;
		}

		auto& device = lights.front();
		device->AuxillaryTraits.Set(AuxillaryStatusTrait{}, ConvertToAuxillaryStatus(msg.State()));
		device->AuxillaryTraits.Set(ColourTrait{}, msg.LightMode());
	}

}
// namespace AqualinkAutomate::Devices
