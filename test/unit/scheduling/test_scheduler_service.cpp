#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/uuid/uuid.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "options/options_scheduling_options.h"
#include "scheduling/schedule.h"
#include "scheduling/scheduler_service.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Scheduling;

namespace
{
	// Recording ICommandDispatcher: counts fires and remembers the last command.
	struct MockDispatcher : Interfaces::ICommandDispatcher
	{
		int calls{ 0 };
		std::string last_kind;
		std::string last_label;
		DeviceAction last_action{ DeviceAction::Toggle };
		int last_value{ 0 };

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string& l) override { ++calls; last_kind = "toggle"; last_label = l; return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string& l, DeviceAction a) override { ++calls; last_kind = "button"; last_label = l; last_action = a; return CommandResult::Success; }
		CommandResult SetPoolSetpoint(uint8_t t) override { ++calls; last_kind = "pool"; last_value = t; return CommandResult::Success; }
		CommandResult SetSpaSetpoint(uint8_t t) override { ++calls; last_kind = "spa"; last_value = t; return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(uint8_t p) override { ++calls; last_kind = "chlor"; last_value = p; return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { ++calls; last_kind = "circ"; return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { ++calls; last_kind = "heater"; return CommandResult::Success; }
		CommandResult SelectIAQPageButton(uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	// Build a schedule aligned to the current local minute of `tp`.
	Schedule ScheduleAt(std::chrono::system_clock::time_point tp, Action action)
	{
		const auto lm = SchedulerService::DecomposeLocal(tp);
		Schedule s;
		s.name = "test";
		s.enabled = true;
		s.days_of_week = static_cast<std::uint8_t>(1u << lm.weekday);
		s.hour = lm.hour;
		s.minute = lm.minute;
		s.action = std::move(action);
		return s;
	}
}

struct SchedulerFixture : Test::HubLocatorInjector
{
	SchedulerFixture()
	{
		mock = std::make_shared<MockDispatcher>();
		this->Register<Interfaces::ICommandDispatcher>(mock);

		settings.schedules_file = (std::filesystem::temp_directory_path() / "aqualink_ws4_schedules_test.json").string();
		std::error_code ec;
		std::filesystem::remove(settings.schedules_file, ec);
	}

	~SchedulerFixture()
	{
		std::error_code ec;
		std::filesystem::remove(settings.schedules_file, ec);
		std::filesystem::remove(settings.schedules_file + ".tmp", ec);
	}

	std::shared_ptr<MockDispatcher> mock;
	Options::Scheduling::SchedulingSettings settings;
};

BOOST_FIXTURE_TEST_SUITE(TestSuite_SchedulerService, SchedulerFixture)

BOOST_AUTO_TEST_CASE(Tick_FiresMatchingScheduleOnce)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Pool Pump", 0 }));

	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls, 1);
	BOOST_CHECK_EQUAL(mock->last_kind, "button");
	BOOST_CHECK_EQUAL(mock->last_label, "Pool Pump");

	// Same minute -> double-fire guard suppresses a second fire.
	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls, 1);
}

BOOST_AUTO_TEST_CASE(Tick_DoesNotFireWrongDay)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	auto schedule = ScheduleAt(now, Action{ ActionType::ButtonToggle, "Pool Pump", 0 });
	// Flip the day mask to everything EXCEPT today.
	schedule.days_of_week = static_cast<std::uint8_t>(~schedule.days_of_week & 0x7F);
	service.Create(schedule);

	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls, 0);
}

BOOST_AUTO_TEST_CASE(Tick_DoesNotFireWhenDisabled)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	auto schedule = ScheduleAt(now, Action{ ActionType::SpaSetpoint, "", 38 });
	schedule.enabled = false;
	service.Create(schedule);

	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls, 0);
}

BOOST_AUTO_TEST_CASE(Tick_SuppressedInServiceMode)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });
	service.Create(ScheduleAt(now, Action{ ActionType::PoolSetpoint, "", 28 }));

	Find<Kernel::DataHub>()->Mode = Kernel::EquipmentMode::Service;

	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls, 0);
}

BOOST_AUTO_TEST_CASE(Crud_PersistsAcrossReload)
{
	const auto now = std::chrono::system_clock::now();

	std::string uuid;
	{
		boost::asio::io_context io;
		SchedulerService service(io, *this, settings);
		auto created = service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Spa Pump", 0 }));
		uuid = created.uuid;
		BOOST_CHECK(!uuid.empty());
		BOOST_CHECK_EQUAL(service.List().size(), 1u);
	}

	// A fresh service reading the same file must load the persisted schedule.
	{
		boost::asio::io_context io;
		SchedulerService service(io, *this, settings);
		service.Start();   // Start() loads from the file
		auto list = service.List();
		BOOST_REQUIRE_EQUAL(list.size(), 1u);
		BOOST_CHECK_EQUAL(list.front().uuid, uuid);
		BOOST_CHECK_EQUAL(list.front().action.target, "Spa Pump");

		BOOST_CHECK(service.Remove(uuid));
		BOOST_CHECK_EQUAL(service.List().size(), 0u);
		service.Stop();
	}
}

BOOST_AUTO_TEST_SUITE_END()
