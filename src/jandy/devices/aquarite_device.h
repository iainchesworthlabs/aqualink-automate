#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "devices/jandy_device.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/restartable.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "messages/aquarite/aquarite_message_getid.h"
#include "messages/aquarite/aquarite_message_percent.h"
#include "messages/aquarite/aquarite_message_ppm.h"
#include "utility/value_debouncer.h"

namespace AqualinkAutomate::Devices
{

	using AquaritePercentage = uint8_t;
	using AquaritePPM = uint16_t;

	struct AquariteGenerating_InPercent
	{
		AquaritePercentage value{};
		std::chrono::system_clock::time_point timestamp{};
	};

	struct AquariteSaltConcentration_InPPM
	{
		AquaritePPM value{};
		std::chrono::system_clock::time_point timestamp{};
	};

	template<typename TYPE_WITH_TIME>
	struct AquariteTypeWithTimeComparator
	{
		constexpr bool operator()(const TYPE_WITH_TIME& p1, const TYPE_WITH_TIME& p2)
		{
			return (p1.value == p2.value);
		}
	};

	class AquariteDevice : public JandyDevice, public Capabilities::Restartable
	{
		inline static const std::chrono::seconds AQUARITE_TIMEOUT_DURATION{ std::chrono::seconds(30) };
		inline static const uint32_t AQUARITE_PERCENT_DEBOUNCE_THRESHOLD{ 10 };

	public:
		using Percentage = AquaritePercentage;
		using PPM = AquaritePPM;
		using Generating_InPercent = AquariteGenerating_InPercent;
		using SaltConcentration_InPPM = AquariteSaltConcentration_InPPM;
		template<typename T> using TypeWithTimeComparator = AquariteTypeWithTimeComparator<T>;

	public:
		AquariteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator);
		AquariteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, Percentage requested_percentage, Percentage reported_percentage, PPM salt_ppm);
		~AquariteDevice() override = default;

	private:
		void WatchdogTimeoutOccurred() override;

	public:
		void RequestedGeneratingLevel(Percentage new_generating_level);
		void ReportedGeneratingLevel(Percentage new_generating_level);
		void ReportedSaltConcentration(PPM new_concentration_level);

		Generating_InPercent RequestedGeneratingLevel() const;
		Generating_InPercent ReportedGeneratingLevel() const;
		SaltConcentration_InPPM ReportedSaltConcentration() const;

	private:
		Utility::ValueDebouncer<Generating_InPercent, TypeWithTimeComparator<Generating_InPercent>> m_Requested;
		Generating_InPercent m_Reported;
		SaltConcentration_InPPM m_SaltPPM;
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };

		// --- Output smoothing (publish-to-DataHub path ONLY) -----------------
		// A borderline AquaRite cell flaps both its salt reading and its low-salt
		// status flag around the cell's own internal threshold.  Observed live
		// (test/fixtures captures): ~87% of frames report "2900 ppm / Low Salt"
		// interleaved with short "4000 ppm / OK" bursts up to ~9 frames long, at
		// roughly 2-3 frames/sec.  The raw Reported* getters above stay
		// unsmoothed; the following state smooths only what is published so the
		// salt graph and the chlorinator-health indicator stop strobing.
		inline static constexpr std::size_t SALT_MEDIAN_WINDOW{ 5 };
		inline static constexpr uint32_t HEALTH_RAISE_FRAMES{ 2 };    // ~1 s: surface a warning/fault quickly
		inline static constexpr uint32_t HEALTH_CLEAR_FRAMES{ 15 };   // ~6 s: clear slowly (sticky)

		std::array<PPM, SALT_MEDIAN_WINDOW> m_SaltWindow{};
		std::size_t m_SaltWindowCount{ 0 };
		std::size_t m_SaltWindowNext{ 0 };

		Kernel::ChlorinatorHealth m_DisplayedHealth{ Kernel::ChlorinatorHealth::Ok };
		Kernel::ChlorinatorHealth m_HealthCandidate{ Kernel::ChlorinatorHealth::Ok };
		uint32_t m_HealthCandidateCount{ 0 };
		bool m_HealthInitialised{ false };

	private:
		void Slot_Aquarite_GetId(const Messages::AquariteMessage_GetId& msg);
		void Slot_Aquarite_Percent(const Messages::AquariteMessage_Percent& msg);
		void Slot_Aquarite_PPM(const Messages::AquariteMessage_PPM& msg);

	private:
		void EnsureChlorinatorDeviceExists();
		void PushPercentToDataHub(const Messages::AquariteMessage_Percent& msg);
		void PushPPMToDataHub(const Messages::AquariteMessage_PPM& msg);

		// Returns the median of the last SALT_MEDIAN_WINDOW raw salt samples,
		// rejecting the cell's transient single-frame ppm spikes.
		PPM MedianFilteredSalt(PPM raw_sample);

		// Asymmetric dwell over the reported health: surface a new warning/fault
		// quickly (HEALTH_RAISE_FRAMES) but clear back to a benign state only
		// after it persists (HEALTH_CLEAR_FRAMES), so a momentary "OK" blip
		// cannot wipe a genuine Low-Salt warning.  Returns the health to publish
		// when the displayed state changes, std::nullopt when it should be held.
		std::optional<Kernel::ChlorinatorHealth> DwellChlorinatorHealth(Kernel::ChlorinatorHealth raw_health);
	};

}
// namespace AqualinkAutomate::Devices
