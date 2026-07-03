#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "alerting/alert_monitor.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Alerting
{
	namespace
	{
		std::int64_t SystemClockSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		// A genuine chlorinator FAULT (not a transient warning).  This condition
		// latches only on the hard-fault states; transient warnings are surfaced
		// by the separate chlorinator_warning condition below.
		bool IsChlorinatorFault(Kernel::ChlorinatorHealth health)
		{
			return health == Kernel::ChlorinatorHealth::Error_CheckPCB
				|| health == Kernel::ChlorinatorHealth::GeneralFault;
		}

		// Human-readable label for a hard-fault health state (used in the alert
		// detail so the log line / UI toast names the specific fault).
		std::string_view ChlorinatorFaultLabel(Kernel::ChlorinatorHealth health)
		{
			switch (health)
			{
			case Kernel::ChlorinatorHealth::Error_CheckPCB: return "Check PCB";
			case Kernel::ChlorinatorHealth::GeneralFault:   return "General fault";
			default:                                        return "Fault";
			}
		}

		// Maps a chlorinator health value to a human-readable WARNING label, or
		// std::nullopt when the value is not a warning (Ok / TurningOff / a hard
		// fault / Unknown).  This is the chlorinator-agnostic catalogue of the
		// cell warnings the AlertMonitor surfaces — every warning the device layer
		// can report, not just low salt.
		std::optional<std::string_view> ChlorinatorWarningLabel(Kernel::ChlorinatorHealth health)
		{
			switch (health)
			{
			case Kernel::ChlorinatorHealth::Warning_NoFlow:         return "No flow";
			case Kernel::ChlorinatorHealth::Warning_LowSalt:        return "Low salt";
			case Kernel::ChlorinatorHealth::Warning_HighSalt:       return "High salt";
			case Kernel::ChlorinatorHealth::Warning_HighCurrent:    return "High current";
			case Kernel::ChlorinatorHealth::Warning_CleanCell:      return "Clean cell";
			case Kernel::ChlorinatorHealth::Warning_LowVoltage:     return "Low voltage";
			case Kernel::ChlorinatorHealth::Warning_LowTemperature: return "Low temperature";
			default:                                                return std::nullopt;
			}
		}

		std::uint64_t TotalMessageCount(const Kernel::StatisticsHub& stats)
		{
			std::uint64_t total = 0;
			for (const auto& entry : stats.MessageCounts)
			{
				total += entry.second.Count();
			}
			return total;
		}
	}
	// unnamed namespace

	AlertMonitor::AlertMonitor(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Alerting::AlertingSettings& settings) :
		m_IoContext(io_context),
		m_Settings(settings),
		m_Clock(SystemClockSeconds),
		m_CommsTimer(io_context)
	{
		m_DataHub = hub_locator.Find<Kernel::DataHub>();
		m_EquipmentHub = hub_locator.Find<Kernel::EquipmentHub>();
		m_StatisticsHub = hub_locator.Find<Kernel::StatisticsHub>();
		m_PreferencesHub = hub_locator.Find<Kernel::PreferencesHub>();

		// Seed the latch for every catalogue condition so transitions are detected
		// from a known (cleared) baseline.
		for (const auto& condition : AlertConditions)
		{
			m_Latched.emplace(std::string{ condition.key }, false);
		}
	}

	AlertMonitor::~AlertMonitor()
	{
		Stop();
	}

	void AlertMonitor::AddSink(Sink sink)
	{
		if (sink)
		{
			m_Sinks.push_back(std::move(sink));
		}
	}

	void AlertMonitor::Start()
	{
		if (m_Running)
		{
			return;
		}
		m_Running = true;

		if (m_DataHub)
		{
			m_DataHubConn = m_DataHub->ConfigUpdateSignal.connect(
				[this](const std::shared_ptr<Kernel::DataHub_ConfigEvent>&) { EvaluateAll(); });
		}

		if (m_EquipmentHub)
		{
			m_EquipmentConn = m_EquipmentHub->EquipmentStatusChangeSignal.connect(
				[this](const std::shared_ptr<Kernel::EquipmentHub_SystemEvent>&) { EvaluateAll(); });
		}

		// Establish the comms baseline so a freshly-started monitor does not raise
		// serial_comms_loss until a full timeout has actually elapsed with no data.
		m_LastMessageCount = m_StatisticsHub ? TotalMessageCount(*m_StatisticsHub) : 0;
		m_LastMessageChangeTs = m_Clock();
		m_HaveCommsBaseline = true;

		EvaluateAll();
		ScheduleCommsTimer();

		LogInfo(Channel::Equipment, "AlertMonitor started");
	}

	void AlertMonitor::Stop()
	{
		if (!m_Running)
		{
			return;
		}
		m_Running = false;

		m_DataHubConn.disconnect();
		m_EquipmentConn.disconnect();

		m_CommsTimer.cancel();
	}

	void AlertMonitor::ScheduleCommsTimer()
	{
		if (!m_Running)
		{
			return;
		}

		// Re-evaluate often enough to detect a comms gap with low latency, without
		// busy-looping; bounded to [1s, 10s].
		const auto tick = std::chrono::seconds(std::max<std::uint32_t>(1, std::min<std::uint32_t>(m_Settings.comms_timeout_seconds, 10)));

		m_CommsTimer.expires_after(tick);
		m_CommsTimer.async_wait(
			[this](const boost::system::error_code& ec)
			{
				if (ec || !m_Running)
				{
					return;
				}
				EvaluateAll();
				ScheduleCommsTimer();
			});
	}

	void AlertMonitor::EvaluateAll()
	{
		EvaluateSaltLow();
		EvaluateChlorinatorWarning();
		EvaluateChlorinatorFault();
		EvaluateServiceMode();
		EvaluateSerialCommsLoss();
		EvaluateTemperatureStale();
	}

	void AlertMonitor::EvaluateSaltLow()
	{
		// Read the threshold LIVE from preferences (seeded from the CLI at boot),
		// so a change via the preferences API takes effect without a restart.
		const std::uint32_t salt_low_ppm = m_PreferencesHub ? m_PreferencesHub->AlertSaltLowPpm : m_Settings.salt_low_ppm;

		if (!m_DataHub || salt_low_ppm == 0)
		{
			// 0 disables the salt check entirely.
			return;
		}

		const double salt = m_DataHub->SaltLevel().value();

		// Salt of exactly 0 means "no sample yet" — never raise on absent data.
		if (salt <= 0.0)
		{
			return;
		}

		const double threshold = static_cast<double>(salt_low_ppm);
		const bool currently_raised = IsRaised(ConditionKeys::SaltLow);

		// Hysteresis: raise below the threshold, clear only once back above
		// threshold + 100 ppm so a reading hovering at the boundary cannot flap.
		if (!currently_raised && salt < threshold)
		{
			SetCondition(ConditionKeys::SaltLow, true,
				std::format("Salt {:.0f} ppm below threshold {:.0f} ppm", salt, threshold),
				{ { "salt_ppm", std::round(salt) }, { "threshold_ppm", threshold } });
		}
		else if (currently_raised && salt >= (threshold + 100.0))
		{
			SetCondition(ConditionKeys::SaltLow, false,
				std::format("Salt {:.0f} ppm recovered above {:.0f} ppm", salt, threshold + 100.0),
				{ { "salt_ppm", std::round(salt) }, { "threshold_ppm", threshold + 100.0 } });
		}
	}

	void AlertMonitor::EvaluateChlorinatorWarning()
	{
		if (!m_DataHub)
		{
			return;
		}

		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			// No chlorinator discovered -> clear any latched warning.
			SetCondition(ConditionKeys::ChlorinatorWarning, false, "No chlorinator present");
			return;
		}

		auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
		std::optional<std::string_view> warning;
		if (health.has_value())
		{
			warning = ChlorinatorWarningLabel(health.value());
		}

		// Raise while the cell is reporting any warning, naming the specific one
		// in the detail so the log line / UI toast is actionable.  The health trait
		// is dwell-smoothed at the device layer, so this does not flap when the
		// cell sits on a boundary.
		SetCondition(ConditionKeys::ChlorinatorWarning, warning.has_value(),
			warning.has_value() ? std::format("Chlorinator warning: {}", warning.value()) : "Chlorinator health OK",
			warning.has_value() ? nlohmann::json{ { "health", magic_enum::enum_name(health.value()) } } : nlohmann::json::object());
	}

	void AlertMonitor::EvaluateChlorinatorFault()
	{
		if (!m_DataHub)
		{
			return;
		}

		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			// No chlorinator discovered -> clear any latched fault.
			SetCondition(ConditionKeys::ChlorinatorFault, false, "No chlorinator present");
			return;
		}

		auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
		const bool fault = health.has_value() && IsChlorinatorFault(health.value());

		SetCondition(ConditionKeys::ChlorinatorFault, fault,
			fault ? std::format("Chlorinator fault: {}", ChlorinatorFaultLabel(health.value())) : "Chlorinator health OK",
			fault ? nlohmann::json{ { "health", magic_enum::enum_name(health.value()) } } : nlohmann::json::object());
	}

	void AlertMonitor::EvaluateServiceMode()
	{
		if (!m_DataHub)
		{
			return;
		}

		const bool in_service = (m_DataHub->Mode == Kernel::EquipmentMode::Service);
		SetCondition(ConditionKeys::ServiceMode, in_service,
			in_service ? "Controller is in Service mode" : "Controller left Service mode",
			in_service ? nlohmann::json{ { "mode", "Service" } } : nlohmann::json::object());
	}

	void AlertMonitor::EvaluateSerialCommsLoss()
	{
		if (!m_StatisticsHub || !m_HaveCommsBaseline)
		{
			return;
		}

		const std::uint64_t count = TotalMessageCount(*m_StatisticsHub);
		const std::int64_t now = m_Clock();

		// Read the timeout LIVE from preferences (seeded from the CLI at boot).
		const std::uint32_t comms_timeout = m_PreferencesHub ? m_PreferencesHub->AlertCommsTimeoutSeconds : m_Settings.comms_timeout_seconds;

		if (count != m_LastMessageCount)
		{
			// Fresh traffic -> note the time and clear any latched loss.
			m_LastMessageCount = count;
			m_LastMessageChangeTs = now;
			SetCondition(ConditionKeys::SerialCommsLoss, false, "Serial traffic resumed");
			return;
		}

		if ((now - m_LastMessageChangeTs) >= static_cast<std::int64_t>(comms_timeout))
		{
			SetCondition(ConditionKeys::SerialCommsLoss, true,
				std::format("No protocol message decoded for {} s", comms_timeout),
				{ { "timeout_seconds", comms_timeout } });
		}
	}

	void AlertMonitor::EvaluateTemperatureStale()
	{
		if (!m_DataHub)
		{
			return;
		}

		// A water temperature going stale is EXPECTED while the pump is off (no flow
		// past the sensor) — the UI ages the reading quietly and no alert is raised.
		// While the pump IS running the active body's sensor should be reporting, so
		// a reading older than the staleness threshold suggests something is wrong
		// (sensor fault, decode gap, wiring).
		bool pump_running = false;
		for (const auto& pump : m_DataHub->FilterPumps())
		{
			auto status = pump->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{});
			if (status.has_value() && Kernel::PumpStatuses::Running == status.value())
			{
				pump_running = true;
				break;
			}
		}

		// The active body is the one the pump circulates — the only body whose sensor
		// can report.  *IsStale() is pure age and false for a never-set reading, so a
		// fresh boot with no reading yet does not raise (serial_comms_loss covers total
		// silence).
		const bool spa_active = m_DataHub->SpaMode();
		const bool is_stale = spa_active ? m_DataHub->SpaTempIsStale() : m_DataHub->PoolTempIsStale();

		const bool raised = pump_running && is_stale;
		const auto threshold_seconds = m_DataHub->TemperatureStalenessThreshold.count();
		const std::string_view body = spa_active ? "spa" : "pool";

		SetCondition(ConditionKeys::TemperatureStale, raised,
			raised
				? std::format("No fresh {} temperature for over {} s while the pump is running", body, threshold_seconds)
				: "Water temperature reporting is fresh",
			raised
				? nlohmann::json{ { "body", body }, { "threshold_seconds", threshold_seconds } }
				: nlohmann::json::object());
	}

	void AlertMonitor::SetCondition(std::string_view key, bool raised, std::string detail, nlohmann::json params)
	{
		auto it = m_Latched.find(key);
		if (it == m_Latched.end())
		{
			it = m_Latched.emplace(std::string{ key }, false).first;
		}

		if (it->second == raised)
		{
			// No edge -> no transition.
			return;
		}

		it->second = raised;

		AlertTransition transition{ std::string{ key }, raised, m_Clock(), std::move(detail), std::move(params) };

		LogInfo(Channel::Equipment, [&] { return std::format("Alert {} {}: {}", transition.condition, raised ? "RAISED" : "cleared", transition.detail); });

		for (const auto& sink : m_Sinks)
		{
			sink(transition);
		}
	}

	bool AlertMonitor::IsRaised(std::string_view condition) const
	{
		auto it = m_Latched.find(condition);
		return it != m_Latched.end() && it->second;
	}

	nlohmann::json AlertMonitor::BuildStateJson() const
	{
		nlohmann::json state = nlohmann::json::object();
		for (const auto& condition : AlertConditions)
		{
			state[std::string{ condition.key }] = IsRaised(condition.key) ? "true" : "false";
		}
		return state;
	}

}
// namespace AqualinkAutomate::Alerting
