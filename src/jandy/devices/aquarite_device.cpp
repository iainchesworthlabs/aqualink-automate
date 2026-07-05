#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <source_location>

#include <magic_enum/magic_enum.hpp>

#include "devices/aquarite_device.h"
#include "devices/device_status.h"
#include "kernel/auxillary_devices/chlorinator_boost_mode.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "types/units_dimensionless.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	namespace
	{
		Kernel::ChlorinatorHealth ConvertToChlorinatorHealthStatus(Messages::AquariteStatuses status)
		{
			switch (status)
			{
			case Messages::AquariteStatuses::On:
			case Messages::AquariteStatuses::Off:                    return Kernel::ChlorinatorHealth::Ok;
			case Messages::AquariteStatuses::TurningOff:             return Kernel::ChlorinatorHealth::TurningOff;
			case Messages::AquariteStatuses::Warning_NoFlow:         return Kernel::ChlorinatorHealth::Warning_NoFlow;
			case Messages::AquariteStatuses::Warning_LowSalt:        return Kernel::ChlorinatorHealth::Warning_LowSalt;
			case Messages::AquariteStatuses::Warning_HighSalt:       return Kernel::ChlorinatorHealth::Warning_HighSalt;
			case Messages::AquariteStatuses::Warning_HighCurrent:    return Kernel::ChlorinatorHealth::Warning_HighCurrent;
			case Messages::AquariteStatuses::Warning_CleanCell:      return Kernel::ChlorinatorHealth::Warning_CleanCell;
			case Messages::AquariteStatuses::Warning_LowVoltage:     return Kernel::ChlorinatorHealth::Warning_LowVoltage;
			case Messages::AquariteStatuses::Warning_LowTemperature: return Kernel::ChlorinatorHealth::Warning_LowTemperature;
			case Messages::AquariteStatuses::Error_CheckPCB:         return Kernel::ChlorinatorHealth::Error_CheckPCB;
			case Messages::AquariteStatuses::GeneralFault:           return Kernel::ChlorinatorHealth::GeneralFault;
			default:                                                 return Kernel::ChlorinatorHealth::Unknown;
			}
		}

		// A "concerning" health is a warning or fault the user should keep seeing;
		// Ok and the transient TurningOff are benign.  Used to make clearing a
		// warning slow (sticky) while raising one stays fast.
		bool IsConcerningHealth(Kernel::ChlorinatorHealth health)
		{
			return !(health == Kernel::ChlorinatorHealth::Ok || health == Kernel::ChlorinatorHealth::TurningOff);
		}
	}
	// namespace

	AquariteDevice::AquariteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator) :
		AquariteDevice(device_id, hub_locator, 0, 0, 0)
	{
	}

	AquariteDevice::AquariteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, Percentage requested_percentage, Percentage reported_percentage, PPM salt_ppm) :
		JandyDevice(device_id),
		Capabilities::Restartable(AQUARITE_TIMEOUT_DURATION),
		m_Requested(AQUARITE_PERCENT_DEBOUNCE_THRESHOLD),
		m_Reported{reported_percentage, std::chrono::system_clock::now()},
		m_SaltPPM{salt_ppm, std::chrono::system_clock::now()}
	{
		m_DataHub = hub_locator.Find<Kernel::DataHub>();

		// Note that this is a debounced value so is initialised differently.
		m_Requested = Generating_InPercent{requested_percentage, std::chrono::system_clock::now()};

		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::AquariteMessage_GetId>([this](auto&& PH1) { Slot_Aquarite_GetId(std::forward<decltype(PH1)>(PH1)); }, (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::AquariteMessage_Percent>([this](auto&& PH1) { Slot_Aquarite_Percent(std::forward<decltype(PH1)>(PH1)); }, (*device_id)());

		// PPM is sent FROM SWG TO Master (destination 0x00), so we cannot filter by our device id (0x50).
		m_SlotManager.RegisterSlot<Messages::AquariteMessage_PPM>([this](auto&& PH1) { Slot_Aquarite_PPM(std::forward<decltype(PH1)>(PH1)); });
	}

	void AquariteDevice::WatchdogTimeoutOccurred()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("AquariteDevice::WatchdogTimeoutOccurred", std::source_location::current());

		LogWarning(Channel::Devices, [this]() { return std::format("Aquarite (0x{:02x}): Watchdog timeout occurred - marking chlorinator as having lost communications.", DeviceId().Id()()); });

		Status(Devices::DeviceStatus_LostComms{});

		// Reset the publish-path smoothing so that, when communications resume,
		// the first fresh reading is shown immediately rather than being held back
		// by the pre-timeout dwell/median history (the watchdog forces the
		// displayed health to Unknown below, bypassing the dwell state).
		m_HealthInitialised = false;
		m_HealthCandidateCount = 0;
		m_SaltWindowCount = 0;
		m_SaltWindowNext = 0;

		// Reflect the loss of communications in the DataHub: the chlorinator is no longer
		// reporting, so drive its operating status Off and health to Unknown rather than
		// leaving the last-seen state stuck active.
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		if (auto chlorinators = m_DataHub->Chlorinators(); !chlorinators.empty())
		{
			auto& device = chlorinators.front();
			device->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
			device->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Unknown);
		}
	}

	void AquariteDevice::RequestedGeneratingLevel(Percentage new_generating_level)
	{
		m_Requested = Generating_InPercent{new_generating_level, std::chrono::system_clock::now()};
	}

	void AquariteDevice::ReportedGeneratingLevel(Percentage new_generating_level)
	{
		m_Reported = Generating_InPercent{new_generating_level, std::chrono::system_clock::now()};
	}

	void AquariteDevice::ReportedSaltConcentration(PPM new_concentration_level)
	{
		m_SaltPPM = SaltConcentration_InPPM{new_concentration_level, std::chrono::system_clock::now()};
	}

	AquariteDevice::Generating_InPercent AquariteDevice::RequestedGeneratingLevel() const
	{
		return m_Requested();
	}

	AquariteDevice::Generating_InPercent AquariteDevice::ReportedGeneratingLevel() const
	{
		return m_Reported;
	}

	AquariteDevice::SaltConcentration_InPPM AquariteDevice::ReportedSaltConcentration() const
	{
		return m_SaltPPM;
	}

	void AquariteDevice::Slot_Aquarite_GetId(const Messages::AquariteMessage_GetId& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("AquariteDevice::Slot_Aquarite_GetId", std::source_location::current());

		LogDebug(Channel::Devices, "Aquarite device received a AquariteMessage_GetId signal.");

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void AquariteDevice::Slot_Aquarite_Percent(const Messages::AquariteMessage_Percent& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("AquariteDevice::Slot_Aquarite_Percent", std::source_location::current());

		LogDebug(Channel::Devices, "Aquarite device received a AquariteMessage_Percent signal.");
		LogDebug(Channel::Devices, [&msg]() { return std::format("Aquarite Device: received new requested generating level -> {}%", msg.GeneratingPercentage()); });
		RequestedGeneratingLevel(msg.GeneratingPercentage());

		PushPercentToDataHub(msg);

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void AquariteDevice::Slot_Aquarite_PPM(const Messages::AquariteMessage_PPM& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("AquariteDevice::Slot_Aquarite_PPM", std::source_location::current());

		LogDebug(Channel::Devices, "Aquarite device received a AquariteMessage_PPM signal.");
		LogDebug(Channel::Devices, [&msg]() { return std::format("Aquarite Device: received new reported status and salt concentration -> {} and {} PPM", magic_enum::enum_name(msg.Status()), msg.SaltConcentrationPPM()); });
		ReportedSaltConcentration(msg.SaltConcentrationPPM());

		PushPPMToDataHub(msg);

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void AquariteDevice::EnsureChlorinatorDeviceExists()
	{
		if (!m_DataHub || !m_DataHub->Chlorinators().empty())
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		LogInfo(Channel::Devices, "AquariteDevice: Creating chlorinator device from AquaRite wire data");

		auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
		ptr->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		ptr->AuxillaryTraits.Set(LabelTrait{}, std::string{"AquaPure"});
		ptr->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
		ptr->AuxillaryTraits.Set(BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
		m_DataHub->Devices.Add(std::move(ptr));
	}

	void AquariteDevice::PushPercentToDataHub(const Messages::AquariteMessage_Percent& msg)
	{
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		EnsureChlorinatorDeviceExists();

		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			return;
		}

		// The wire byte multiplexes sentinels with the generating percentage: 101 means
		// Boost and 255 means Service Mode.  Those sentinels must NOT leak into the
		// 0-100 percentage traits, so clamp the displayed duty-cycle / generating level
		// to a real percentage and surface the sentinels through dedicated traits.
		const uint8_t raw_percentage = msg.GeneratingPercentage();
		const uint8_t clamped_percentage = msg.IsServiceMode()
			? static_cast<uint8_t>(0)
			: std::min<uint8_t>(raw_percentage, static_cast<uint8_t>(100));

		auto& device = chlorinators.front();
		device->AuxillaryTraits.Set(GeneratingPercentageTrait{}, clamped_percentage);
		device->AuxillaryTraits.Set(BoostModeTrait{}, msg.IsBoostMode() ? Kernel::ChlorinatorBoostModes::Boost : Kernel::ChlorinatorBoostModes::Off);
		device->AuxillaryTraits.Set(DutyCycleTrait{}, clamped_percentage);

		// Passive fallback for the dashboard target: remember the last REAL, non-zero
		// generating % - a genuine setpoint while the cell runs - so the configured value
		// survives the cell going idle (0x11 -> 0) when no authoritative menu scrape is
		// available.  Boost (101) and Service (255) are sentinels, not setpoints, so they
		// are excluded; a non-boost, non-service value in 1..100 is a real output %.
		if (!msg.IsBoostMode() && !msg.IsServiceMode() && clamped_percentage >= 1)
		{
			device->AuxillaryTraits.Set(ChlorinatorLastGeneratingTrait{}, clamped_percentage);
		}
	}

	AquariteDevice::PPM AquariteDevice::MedianFilteredSalt(PPM raw_sample)
	{
		m_SaltWindow[m_SaltWindowNext] = raw_sample;
		m_SaltWindowNext = (m_SaltWindowNext + 1) % SALT_MEDIAN_WINDOW;
		if (m_SaltWindowCount < SALT_MEDIAN_WINDOW)
		{
			++m_SaltWindowCount;
		}

		// Median of the populated samples.  For an even count this returns the
		// upper-middle element, which is an acceptable median proxy; the window is
		// odd-sized and fills within a few frames, so most readings are an exact
		// median.
		std::array<PPM, SALT_MEDIAN_WINDOW> scratch = m_SaltWindow;
		const auto first = scratch.begin();
		const auto last = first + static_cast<std::ptrdiff_t>(m_SaltWindowCount);
		const auto middle = first + static_cast<std::ptrdiff_t>(m_SaltWindowCount / 2);
		std::nth_element(first, middle, last);
		return *middle;
	}

	std::optional<Kernel::ChlorinatorHealth> AquariteDevice::DwellChlorinatorHealth(Kernel::ChlorinatorHealth raw_health)
	{
		// Never let an undecodable status (Unknown) disturb the displayed health:
		// hold the last shown value rather than flashing "Unknown".
		if (raw_health == Kernel::ChlorinatorHealth::Unknown)
		{
			return std::nullopt;
		}

		// The first decodable reading is shown immediately; the dwell governs only
		// subsequent changes.
		if (!m_HealthInitialised)
		{
			m_HealthInitialised = true;
			m_DisplayedHealth = raw_health;
			m_HealthCandidate = raw_health;
			m_HealthCandidateCount = 0;
			return raw_health;
		}

		if (raw_health == m_DisplayedHealth)
		{
			// Reading agrees with what we already show -> abandon any pending change.
			m_HealthCandidate = raw_health;
			m_HealthCandidateCount = 0;
			return std::nullopt;
		}

		// A candidate change: count consecutive frames advocating the same new state.
		if (raw_health == m_HealthCandidate)
		{
			++m_HealthCandidateCount;
		}
		else
		{
			m_HealthCandidate = raw_health;
			m_HealthCandidateCount = 1;
		}

		// Clearing a warning/fault back to a benign state is deliberately slow
		// (sticky); raising or switching between concerning states is fast.
		const bool is_clearing = IsConcerningHealth(m_DisplayedHealth) && !IsConcerningHealth(raw_health);
		if (const uint32_t threshold = is_clearing ? HEALTH_CLEAR_FRAMES : HEALTH_RAISE_FRAMES; m_HealthCandidateCount >= threshold)
		{
			m_DisplayedHealth = raw_health;
			m_HealthCandidateCount = 0;
			return raw_health;
		}

		return std::nullopt;
	}

	void AquariteDevice::PushPPMToDataHub(const Messages::AquariteMessage_PPM& msg)
	{
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;
		using namespace Units;

		// Publish a median-smoothed salt level so the cell's transient ppm spikes
		// do not sawtooth the salt graph / salt-low alert.
		const PPM smoothed_salt = MedianFilteredSalt(msg.SaltConcentrationPPM());
		m_DataHub->SaltLevel(static_cast<double>(smoothed_salt) * Units::ppm);

		EnsureChlorinatorDeviceExists();

		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			return;
		}

		auto& device = chlorinators.front();

		// Apply the asymmetric dwell and only write the health trait on a real
		// (dwelled) transition, so the indicator stops flapping between OK and a
		// momentary warning when the cell sits on its low-salt boundary.
		if (auto displayed = DwellChlorinatorHealth(ConvertToChlorinatorHealthStatus(msg.Status())); displayed.has_value())
		{
			device->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, displayed.value());
		}

		// Operating status (On/Off) follows the raw wire status directly: it is a
		// distinct, stable signal that does not exhibit the boundary flapping.
		auto operating_status = (msg.Status() == Messages::AquariteStatuses::Off || msg.Status() == Messages::AquariteStatuses::TurningOff)
			? Kernel::ChlorinatorStatuses::Off
			: Kernel::ChlorinatorStatuses::On;
		device->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, operating_status);
	}

}
// namespace AqualinkAutomate::Devices
