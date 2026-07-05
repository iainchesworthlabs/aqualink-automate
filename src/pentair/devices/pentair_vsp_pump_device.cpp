#include <format>
#include <source_location>

#include "devices/pentair_vsp_pump_device.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "messages/pentair_message_constants.h"
#include "messages/pump/pentair_pump_message_power.h"
#include "messages/pump/pentair_pump_message_speed.h"
#include "profiling/profiling.h"
#include "types/units_flow_rate.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

namespace Capabilities = AqualinkAutomate::Devices::Capabilities;

namespace AqualinkAutomate::Pentair::Devices
{

	PentairVSPPumpDevice::PentairVSPPumpDevice(const std::shared_ptr<PentairDeviceId>& device_id, Kernel::HubLocator& hub_locator) :
		PentairDevice(device_id),
		Capabilities::Restartable(PUMP_TIMEOUT_DURATION),
		m_Address((nullptr != device_id) ? (*device_id)() : static_cast<uint8_t>(0)),
		m_DataHub(hub_locator.Find<Kernel::DataHub>())
	{
		m_SlotManager.RegisterSlot<Messages::PentairPumpMessage_Status>(
			[this](const Messages::PentairPumpMessage_Status& msg) { Slot_Pump_Status(msg); });
	}

	PentairVSPPumpDevice::TimestampedRPM PentairVSPPumpDevice::ReportedRPM() const
	{
		return m_RPM;
	}

	PentairVSPPumpDevice::TimestampedWatts PentairVSPPumpDevice::ReportedWatts() const
	{
		return m_Watts;
	}

	PentairVSPPumpDevice::TimestampedGPM PentairVSPPumpDevice::ReportedGPM() const
	{
		return m_GPM;
	}

	bool PentairVSPPumpDevice::IsRunning() const
	{
		return m_IsRunning;
	}

	void PentairVSPPumpDevice::SetSpeed(uint16_t rpm) const
	{
		// CAPTURE-GATED: the command emitters are validated by round-trip unit
		// tests but are not yet wired to any production caller (no command path
		// reaches the Pentair pump on a live bus).  Kept for the command plumbing
		// and to be hooked up once a live capture confirms the addressing.
		LogInfo(Channel::Devices, [this, rpm] { return std::format("Pentair VSP pump (0x{:02x}): emitting set-speed command -> {} RPM", m_Address, rpm); });
		Messages::PentairPumpMessage_Speed command(Messages::CONTROLLER_ADDRESS, m_Address, rpm);
		command.Signal_MessageToSend();
	}

	void PentairVSPPumpDevice::SetPower(bool power_on) const
	{
		// CAPTURE-GATED: see SetSpeed().  Not yet reachable from production.
		LogInfo(Channel::Devices, [this, power_on] { return std::format("Pentair VSP pump (0x{:02x}): emitting power command -> {}", m_Address, power_on ? "ON" : "OFF"); });
		Messages::PentairPumpMessage_Power command(Messages::CONTROLLER_ADDRESS, m_Address, power_on);
		command.Signal_MessageToSend();
	}

	void PentairVSPPumpDevice::WatchdogTimeoutOccurred()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PentairVSPPumpDevice::WatchdogTimeoutOccurred", std::source_location::current());

		LogWarning(Channel::Devices, [this] { return std::format("Pentair VSP pump (0x{:02x}) watchdog expired; marking not running", m_Address); });
		m_IsRunning = false;

		// The pump has stopped reporting: clear the live operating point so stale
		// RPM/Watts/GPM readings are not presented as current.
		const auto now = std::chrono::system_clock::now();
		m_RPM = std::make_pair(static_cast<uint16_t>(0), now);
		m_Watts = std::make_pair(static_cast<uint16_t>(0), now);
		m_GPM = std::make_pair(static_cast<uint8_t>(0), now);

		if (auto device = ResolvePumpDevice(); nullptr != device)
		{
			device->AuxillaryTraits.Set(PumpStatusTrait{}, Kernel::PumpStatuses::Off);
			device->AuxillaryTraits.Set(FlowRateTrait{}, Kernel::FlowRate(0.0 * Units::gpm));
		}
	}

	void PentairVSPPumpDevice::Slot_Pump_Status(const Messages::PentairPumpMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PentairVSPPumpDevice::Slot_Pump_Status", std::source_location::current());

		// Status frames are broadcast by the pump; match on the FROM address.
		if (msg.From() != m_Address)
		{
			return;
		}

		LogDebug(Channel::Devices, [this, &msg] { return std::format("Pentair VSP pump (0x{:02x}) status: RPM {}, Watts {}, GPM {}, Running {}",
			m_Address, msg.RPM(), msg.Watts(), msg.GPM(), msg.IsRunning()); });

		const auto now = std::chrono::system_clock::now();
		m_RPM = std::make_pair(msg.RPM(), now);
		m_Watts = std::make_pair(msg.Watts(), now);
		m_GPM = std::make_pair(msg.GPM(), now);
		m_IsRunning = msg.IsRunning();

		PushStatusToDataHub(msg);

		Capabilities::Restartable::Kick();
	}

	std::shared_ptr<Kernel::AuxillaryDevice> PentairVSPPumpDevice::ResolvePumpDevice()
	{
		if (!m_DataHub)
		{
			return nullptr;
		}

		// Fast path: we already know this device's uuid.
		if (m_PumpDeviceId.has_value())
		{
			if (auto device = m_DataHub->Devices.FindById(m_PumpDeviceId.value()); nullptr != device)
			{
				return device;
			}

			// Cached id no longer resolves (device removed); fall through and
			// re-create so the cache self-heals.
			m_PumpDeviceId.reset();
		}

		// Create the pump auxillary once, key it by the address-derived label, and
		// cache its identity so future lookups are O(1).
		auto device = std::make_shared<Kernel::AuxillaryDevice>();
		device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Pump);
		device->AuxillaryTraits.Set(LabelTrait{}, std::format("Pentair Pump 0x{:02x}", m_Address));
		device->AuxillaryTraits.Set(PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);
		device->AuxillaryTraits.Set(PumpSpeedTrait{}, Kernel::PumpSpeeds::VariableSpeed);
		device->AuxillaryTraits.Set(PumpStatusTrait{}, Kernel::PumpStatuses::Off);

		m_PumpDeviceId = device->Id();
		m_DataHub->Devices.Add(device);

		return device;
	}

	void PentairVSPPumpDevice::PushStatusToDataHub(const Messages::PentairPumpMessage_Status& msg)
	{
		auto device = ResolvePumpDevice();
		if (nullptr == device)
		{
			return;
		}

		device->AuxillaryTraits.Set(PumpStatusTrait{}, msg.IsRunning() ? Kernel::PumpStatuses::Running : Kernel::PumpStatuses::Off);
		device->AuxillaryTraits.Set(FlowRateTrait{}, Kernel::FlowRate(static_cast<double>(msg.GPM()) * Units::gpm));
	}

}
// namespace AqualinkAutomate::Pentair::Devices
