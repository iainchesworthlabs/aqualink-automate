#include <format>
#include <source_location>

#include "devices/pentair_chlorinator_device.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "messages/chlorinator/pentair_chlorinator_message_setoutput.h"
#include "messages/pentair_message_constants.h"
#include "profiling/profiling.h"
#include "types/units_dimensionless.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

namespace Capabilities = AqualinkAutomate::Devices::Capabilities;

namespace AqualinkAutomate::Pentair::Devices
{

	namespace
	{
		Kernel::ChlorinatorHealth FlagsToHealth(const Messages::PentairChlorinatorMessage_Status& msg)
		{
			using enum Kernel::ChlorinatorHealth;
			if (msg.IsLowFlow())      { return Warning_NoFlow; }
			if (msg.IsLowSalt())      { return Warning_LowSalt; }
			if (msg.IsHighSalt())     { return Warning_HighSalt; }
			if (msg.NeedsCleanCell()) { return Warning_CleanCell; }
			return Ok;
		}

		constexpr char CHLORINATOR_LABEL[] = "IntelliChlor";
	}

	PentairChlorinatorDevice::PentairChlorinatorDevice(const std::shared_ptr<PentairDeviceId>& device_id, Kernel::HubLocator& hub_locator) :
		PentairDevice(device_id),
		Capabilities::Restartable(CHLORINATOR_TIMEOUT_DURATION),
		m_Address((nullptr != device_id) ? (*device_id)() : static_cast<uint8_t>(0)),
		m_DataHub(hub_locator.Find<Kernel::DataHub>())
	{
		m_SlotManager.RegisterSlot<Messages::PentairChlorinatorMessage_Status>(
			[this](const Messages::PentairChlorinatorMessage_Status& msg) { Slot_Chlorinator_Status(msg); });
	}

	uint16_t PentairChlorinatorDevice::ReportedSaltPPM() const
	{
		return m_SaltPPM;
	}

	uint8_t PentairChlorinatorDevice::ReportedOutputPercent() const
	{
		return m_OutputPercent;
	}

	void PentairChlorinatorDevice::SetOutput(uint8_t output_percent) const
	{
		// CAPTURE-GATED: validated by round-trip unit tests but not yet wired to a
		// production caller; retained for the command plumbing.
		LogInfo(Channel::Devices, [this, output_percent] { return std::format("Pentair IntelliChlor (0x{:02x}): emitting set-output command -> {}%", m_Address, output_percent); });
		Messages::PentairChlorinatorMessage_SetOutput command(Messages::CONTROLLER_ADDRESS, m_Address, output_percent);
		command.Signal_MessageToSend();
	}

	void PentairChlorinatorDevice::WatchdogTimeoutOccurred()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PentairChlorinatorDevice::WatchdogTimeoutOccurred", std::source_location::current());

		LogWarning(Channel::Devices, [this] { return std::format("Pentair IntelliChlor (0x{:02x}) watchdog expired", m_Address); });
	}

	void PentairChlorinatorDevice::Slot_Chlorinator_Status(const Messages::PentairChlorinatorMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("PentairChlorinatorDevice::Slot_Chlorinator_Status", std::source_location::current());

		// Chlorinator status is broadcast by the SWG; match on the FROM address.
		if (msg.From() != m_Address)
		{
			return;
		}

		LogDebug(Channel::Devices, [this, &msg] { return std::format("Pentair IntelliChlor (0x{:02x}) status: salt {} PPM, output {}%, flags 0x{:02x}",
			m_Address, msg.SaltPPM(), msg.OutputPercent(), msg.StatusFlags()); });

		m_SaltPPM = msg.SaltPPM();
		m_OutputPercent = msg.OutputPercent();

		PushStatusToDataHub(msg);

		Capabilities::Restartable::Kick();
	}

	void PentairChlorinatorDevice::EnsureChlorinatorDeviceExists() const
	{
		if (!m_DataHub || !m_DataHub->Chlorinators().empty())
		{
			return;
		}

		auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
		ptr->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		ptr->AuxillaryTraits.Set(LabelTrait{}, std::string{ CHLORINATOR_LABEL });
		ptr->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);

		m_DataHub->Devices.Add(std::move(ptr));
	}

	void PentairChlorinatorDevice::PushStatusToDataHub(const Messages::PentairChlorinatorMessage_Status& msg)
	{
		if (!m_DataHub)
		{
			return;
		}

		m_DataHub->SaltLevel(static_cast<double>(msg.SaltPPM()) * Units::ppm);

		EnsureChlorinatorDeviceExists();

		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			return;
		}

		auto& device = chlorinators.front();
		device->AuxillaryTraits.Set(GeneratingPercentageTrait{}, msg.OutputPercent());
		device->AuxillaryTraits.Set(DutyCycleTrait{}, msg.OutputPercent());
		device->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, FlagsToHealth(msg));
		device->AuxillaryTraits.Set(ChlorinatorStatusTrait{},
			(msg.OutputPercent() > 0) ? Kernel::ChlorinatorStatuses::On : Kernel::ChlorinatorStatuses::Off);
	}

}
// namespace AqualinkAutomate::Pentair::Devices
