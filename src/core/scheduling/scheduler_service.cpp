#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "logging/logging.h"
#include "platform/safe_ctime.h"
#include "scheduling/scheduler_service.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Scheduling
{
	namespace
	{
		std::chrono::system_clock::time_point SystemNow()
		{
			return std::chrono::system_clock::now();
		}

		std::string NewUuid()
		{
			boost::uuids::random_generator gen;
			return boost::uuids::to_string(gen());
		}
	}
	// unnamed namespace

	SchedulerService::SchedulerService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Scheduling::SchedulingSettings& settings) :
		m_IoContext(io_context),
		m_Settings(settings),
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_Dispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>()),
		m_Clock(SystemNow),
		m_Timer(io_context)
	{
	}

	SchedulerService::~SchedulerService()
	{
		Stop();
	}

	void SchedulerService::Start()
	{
		if (m_Running || m_Settings.schedules_file.empty())
		{
			return;
		}

		m_Running = true;
		Load();
		ScheduleTick();

		LogInfo(Channel::Main, [this] { return std::format("Scheduler started ({} schedule(s), file '{}')", m_Schedules.size(), m_Settings.schedules_file); });
	}

	void SchedulerService::Stop()
	{
		if (!m_Running)
		{
			return;
		}
		m_Running = false;
		m_Timer.cancel();
	}

	SchedulerService::LocalMinute SchedulerService::DecomposeLocal(std::chrono::system_clock::time_point tp)
	{
		const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
		std::tm tm{};
		(void)Platform::SafeLocalTime(tm, tt);
		LocalMinute lm;
		// tm_wday is 0=Sunday..6=Saturday; remap to Monday=0..Sunday=6.
		lm.weekday = (tm.tm_wday + 6) % 7;
		lm.hour = tm.tm_hour;
		lm.minute = tm.tm_min;
		lm.key = (static_cast<std::int64_t>(tm.tm_year + 1900) * 100000000)
			+ (static_cast<std::int64_t>(tm.tm_mon + 1) * 1000000)
			+ (static_cast<std::int64_t>(tm.tm_mday) * 10000)
			+ (static_cast<std::int64_t>(tm.tm_hour) * 100)
			+ tm.tm_min;
		return lm;
	}

	void SchedulerService::Tick()
	{
		const auto lm = DecomposeLocal(m_Clock());

		for (const auto& schedule : m_Schedules)
		{
			if (!schedule.enabled)
			{
				continue;
			}

			if (const bool day_matches = (schedule.days_of_week & (1u << lm.weekday)) != 0; !day_matches || schedule.hour != lm.hour || schedule.minute != lm.minute)
			{
				continue;
			}

			// Already fired this exact minute -> skip (double-fire guard).
			if (auto it = m_LastFiredMinute.find(schedule.uuid); it != m_LastFiredMinute.end() && it->second == lm.key)
			{
				continue;
			}

			m_LastFiredMinute[schedule.uuid] = lm.key;
			Fire(schedule);
		}
	}

	void SchedulerService::Fire(const Schedule& schedule)
	{
		// Service mode suppresses automation.
		if (m_DataHub && m_DataHub->Mode == Kernel::EquipmentMode::Service)
		{
			LogWarning(Channel::Main, [&schedule] { return std::format("Scheduler: skipping '{}' — controller is in Service mode", schedule.name); });
			return;
		}

		if (!m_Dispatcher)
		{
			LogWarning(Channel::Main, [&schedule] { return std::format("Scheduler: cannot fire '{}' — no command dispatcher available", schedule.name); });
			return;
		}

		using DA = Interfaces::ICommandDispatcher::DeviceAction;
		Interfaces::ICommandDispatcher::CommandResult result{ Interfaces::ICommandDispatcher::CommandResult::InvalidValue };

		switch (schedule.action.type)
		{
		case ActionType::ButtonOn:
			result = m_Dispatcher->CommandByLabel(schedule.action.target, DA::On);
			break;
		case ActionType::ButtonOff:
			result = m_Dispatcher->CommandByLabel(schedule.action.target, DA::Off);
			break;
		case ActionType::ButtonToggle:
			result = m_Dispatcher->CommandByLabel(schedule.action.target, DA::Toggle);
			break;
		case ActionType::PoolSetpoint:
			result = m_Dispatcher->SetPoolSetpoint(static_cast<std::uint8_t>(schedule.action.value));
			break;
		case ActionType::SpaSetpoint:
			result = m_Dispatcher->SetSpaSetpoint(static_cast<std::uint8_t>(schedule.action.value));
			break;
		case ActionType::ChlorinatorPercent:
			result = m_Dispatcher->SetChlorinatorPercentage(static_cast<std::uint8_t>(schedule.action.value));
			break;
		case ActionType::CirculationMode:
			if (auto mode = magic_enum::enum_cast<Kernel::CirculationModes>(schedule.action.target); mode.has_value())
			{
				result = m_Dispatcher->SetCirculationMode(mode.value());
			}
			break;
		}

		LogInfo(Channel::Main, [&schedule, &result] { return std::format("Scheduler fired '{}' ({}) -> {}", schedule.name, ActionTypeToString(schedule.action.type), magic_enum::enum_name(result)); });
	}

	void SchedulerService::Load()
	{
		m_Schedules.clear();

		if (!std::filesystem::exists(m_Settings.schedules_file))
		{
			return;
		}

		try
		{
			std::ifstream in(m_Settings.schedules_file, std::ios::binary);
			nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
			if (!json.is_array())
			{
				LogWarning(Channel::Main, "Scheduler: schedules file is not a JSON array; ignoring");
				return;
			}

			for (const auto& entry : json)
			{
				std::string error;
				if (auto schedule = FromJson(entry, error); schedule.has_value())
				{
					if (schedule->uuid.empty()) { schedule->uuid = NewUuid(); }
					m_Schedules.push_back(std::move(*schedule));
				}
				else
				{
					// Tolerate one bad entry rather than rejecting the whole file.
					LogWarning(Channel::Main, [&error] { return std::format("Scheduler: skipping invalid schedule entry: {}", error); });
				}
			}
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&ex] { return std::format("Scheduler: failed to load schedules file: {}", ex.what()); });
		}
	}

	void SchedulerService::Save()
	{
		if (m_Settings.schedules_file.empty())
		{
			return;
		}

		try
		{
			nlohmann::json array = nlohmann::json::array();
			for (const auto& schedule : m_Schedules)
			{
				array.push_back(ToJson(schedule));
			}

			// Atomic write: temp file then rename over the target.
			const std::filesystem::path target(m_Settings.schedules_file);
			const std::filesystem::path tmp = target.string() + ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out << array.dump(2);
			}
			std::filesystem::rename(tmp, target);
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&ex] { return std::format("Scheduler: failed to save schedules file: {}", ex.what()); });
		}
	}

	std::vector<Schedule> SchedulerService::List() const
	{
		return m_Schedules;
	}

	std::optional<Schedule> SchedulerService::Get(const std::string& uuid) const
	{
		for (const auto& schedule : m_Schedules)
		{
			if (schedule.uuid == uuid) { return schedule; }
		}
		return std::nullopt;
	}

	Schedule SchedulerService::Create(Schedule schedule)
	{
		schedule.uuid = NewUuid();
		m_Schedules.push_back(schedule);
		Save();
		return schedule;
	}

	bool SchedulerService::Replace(const std::string& uuid, Schedule schedule)
	{
		for (auto& existing : m_Schedules)
		{
			if (existing.uuid == uuid)
			{
				schedule.uuid = uuid;
				existing = std::move(schedule);
				Save();
				return true;
			}
		}
		return false;
	}

	bool SchedulerService::Remove(const std::string& uuid)
	{
		for (auto it = m_Schedules.begin(); it != m_Schedules.end(); ++it)
		{
			if (it->uuid == uuid)
			{
				m_Schedules.erase(it);
				m_LastFiredMinute.erase(uuid);
				Save();
				return true;
			}
		}
		return false;
	}

	void SchedulerService::ScheduleTick()
	{
		if (!m_Running)
		{
			return;
		}

		// Tick every 30s so the matching minute is never missed by more than that;
		// the per-minute double-fire guard makes the sub-minute cadence safe.
		m_Timer.expires_after(std::chrono::seconds(30));
		m_Timer.async_wait([this](const boost::system::error_code& ec)
		{
			if (ec || !m_Running) { return; }
			Tick();
			ScheduleTick();
		});
	}

}
// namespace AqualinkAutomate::Scheduling
