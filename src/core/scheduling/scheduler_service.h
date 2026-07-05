#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "options/options_scheduling_options.h"
#include "scheduling/schedule.h"

namespace AqualinkAutomate::Kernel { class DataHub; class HubLocator; }
namespace AqualinkAutomate::Interfaces { class ICommandDispatcher; }

namespace AqualinkAutomate::Scheduling
{

	//=========================================================================
	// SchedulerService — a clock-driven engine that fires existing dispatcher
	// commands at local wall-clock times.
	//
	// A one-minute asio timer recomputes the current LOCAL minute on each tick
	// and fires every enabled schedule whose day-of-week + HH:MM match the minute
	// just entered.  Recomputing from local time each tick makes DST handling
	// automatic (a skipped spring-forward minute simply never fires; an ambiguous
	// fall-back minute fires once).  A per-schedule last-fired-minute guard
	// prevents a double fire within the same minute.  Service mode suppresses
	// firing.  The clock is injectable for deterministic tests.
	//
	// Schedules persist as a JSON array at --schedules-file (atomic
	// write-temp-then-rename); empty path => scheduler disabled.
	//=========================================================================
	class SchedulerService
	{
	public:
		using ClockFn = std::function<std::chrono::system_clock::time_point()>;

		SchedulerService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Scheduling::SchedulingSettings& settings);
		~SchedulerService();

		SchedulerService(const SchedulerService&) = delete;
		SchedulerService& operator=(const SchedulerService&) = delete;

		void Start();   // load schedules + start the per-minute timer
		void Stop();

		// CRUD (used by the routes). Mutations persist to disk.
		std::vector<Schedule> List() const;
		std::optional<Schedule> Get(const std::string& uuid) const;
		Schedule Create(Schedule schedule);              // assigns a uuid
		bool Replace(const std::string& uuid, Schedule schedule);
		bool Remove(const std::string& uuid);

		// Engine tick (public for tests). Evaluates all schedules for the current
		// local minute and fires the matching, not-yet-fired ones.
		void Tick();

		void SetClock(ClockFn clock) { m_Clock = std::move(clock); }

		// Decompose a time_point into the local (weekday Mon=0..Sun=6, hour,
		// minute) and a unique-per-minute key. Exposed so tests can align a
		// schedule to an injected clock without depending on the host timezone.
		struct LocalMinute { int weekday{ 0 }; int hour{ 0 }; int minute{ 0 }; std::int64_t key{ 0 }; };
		static LocalMinute DecomposeLocal(std::chrono::system_clock::time_point tp);

	private:
		void Load();
		void Save();
		void ScheduleTick();
		void Fire(const Schedule& schedule);

	private:
		boost::asio::io_context& m_IoContext;
		Options::Scheduling::SchedulingSettings m_Settings;
		std::shared_ptr<Kernel::DataHub> m_DataHub;
		std::shared_ptr<Interfaces::ICommandDispatcher> m_Dispatcher;

		std::vector<Schedule> m_Schedules;
		std::map<std::string, std::int64_t> m_LastFiredMinute;  // uuid -> minute key

		ClockFn m_Clock;
		bool m_Running{ false };
		boost::asio::steady_timer m_Timer;
	};

}
// namespace AqualinkAutomate::Scheduling
