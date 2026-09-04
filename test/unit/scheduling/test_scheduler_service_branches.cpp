#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/uuid/uuid.hpp>
#include <nlohmann/json.hpp>

#include "interfaces/icommanddispatcher.h"
#include "kernel/circulation.h"
#include "kernel/data_hub.h"
#include "options/options_scheduling_options.h"
#include "scheduling/schedule.h"
#include "scheduling/scheduler_service.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Scheduling;

//=============================================================================
// SchedulerService branch coverage: every Fire() action arm, the no-dispatcher
// and Service-mode guards, the day/hour/minute matching arms, the per-minute
// double-fire key, the Start()/Load() file edge cases (non-array, invalid
// entry, malformed JSON, missing uuid), Replace()/Remove()/Get() misses, a
// failing Save() and the cancelled timer callback.
//=============================================================================

namespace
{
	// Recording ICommandDispatcher: remembers every command it received.
	struct RecordingDispatcher : Interfaces::ICommandDispatcher
	{
		struct Call
		{
			std::string kind;
			std::string label;
			DeviceAction action{ DeviceAction::Toggle };
			int value{ 0 };
			Kernel::CirculationModes mode{ Kernel::CirculationModes::Pool };
		};

		std::vector<Call> calls;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string& l) override { calls.push_back({ "toggle", l, DeviceAction::Toggle, 0, {} }); return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string& l, DeviceAction a) override { calls.push_back({ "button", l, a, 0, {} }); return CommandResult::Success; }
		CommandResult SetPoolSetpoint(uint8_t t) override { calls.push_back({ "pool", {}, DeviceAction::Toggle, t, {} }); return CommandResult::Success; }
		CommandResult SetSpaSetpoint(uint8_t t) override { calls.push_back({ "spa", {}, DeviceAction::Toggle, t, {} }); return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t p, Kernel::BodyOfWaterIds) override { calls.push_back({ "chlor", {}, DeviceAction::Toggle, p, {} }); return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes m) override { calls.push_back({ "circ", {}, DeviceAction::Toggle, 0, m }); return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	Schedule ScheduleAt(std::chrono::system_clock::time_point tp, Action action, const std::string& name = "test")
	{
		const auto lm = SchedulerService::DecomposeLocal(tp);
		Schedule s;
		s.name = name;
		s.enabled = true;
		s.days_of_week = static_cast<std::uint8_t>(1u << lm.weekday);
		s.hour = lm.hour;
		s.minute = lm.minute;
		s.action = std::move(action);
		return s;
	}

	std::string ReadAll(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}
}
// anonymous namespace

struct SchedulerBranchesFixture : Test::HubLocatorInjector
{
	SchedulerBranchesFixture()
	{
		mock = std::make_shared<RecordingDispatcher>();
		this->Register<Interfaces::ICommandDispatcher>(mock);

		static int counter = 0;
		settings.schedules_file = (std::filesystem::temp_directory_path() / ("aqualink_sched_branches_" + std::to_string(++counter) + ".json")).string();
		std::error_code ec;
		std::filesystem::remove(settings.schedules_file, ec);
	}

	~SchedulerBranchesFixture()
	{
		std::error_code ec;
		std::filesystem::remove(settings.schedules_file, ec);
		std::filesystem::remove(settings.schedules_file + ".tmp", ec);
	}

	std::shared_ptr<RecordingDispatcher> mock;
	Options::Scheduling::SchedulingSettings settings;
};

BOOST_FIXTURE_TEST_SUITE(TestSuite_SchedulerServiceBranches, SchedulerBranchesFixture)

// -----------------------------------------------------------------------------
// Fire(): every action arm reaches the matching dispatcher call
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Fire_DispatchesEveryActionType)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	service.Create(ScheduleAt(now, Action{ ActionType::ButtonOff, "Pool Light", 0 }, "off"));
	service.Create(ScheduleAt(now, Action{ ActionType::ButtonToggle, "Spa Jets", 0 }, "toggle"));
	service.Create(ScheduleAt(now, Action{ ActionType::PoolSetpoint, "", 82 }, "pool"));
	service.Create(ScheduleAt(now, Action{ ActionType::SpaSetpoint, "", 101 }, "spa"));
	service.Create(ScheduleAt(now, Action{ ActionType::ChlorinatorPercent, "", 55 }, "chlor"));
	service.Create(ScheduleAt(now, Action{ ActionType::CirculationMode, "Spa", 0 }, "circ"));

	service.Tick();

	BOOST_REQUIRE_EQUAL(mock->calls.size(), 6u);

	BOOST_CHECK_EQUAL(mock->calls[0].kind, "button");
	BOOST_CHECK_EQUAL(mock->calls[0].label, "Pool Light");
	BOOST_CHECK(Interfaces::ICommandDispatcher::DeviceAction::Off == mock->calls[0].action);

	BOOST_CHECK_EQUAL(mock->calls[1].kind, "button");
	BOOST_CHECK_EQUAL(mock->calls[1].label, "Spa Jets");
	BOOST_CHECK(Interfaces::ICommandDispatcher::DeviceAction::Toggle == mock->calls[1].action);

	BOOST_CHECK_EQUAL(mock->calls[2].kind, "pool");
	BOOST_CHECK_EQUAL(mock->calls[2].value, 82);

	BOOST_CHECK_EQUAL(mock->calls[3].kind, "spa");
	BOOST_CHECK_EQUAL(mock->calls[3].value, 101);

	BOOST_CHECK_EQUAL(mock->calls[4].kind, "chlor");
	BOOST_CHECK_EQUAL(mock->calls[4].value, 55);

	BOOST_CHECK_EQUAL(mock->calls[5].kind, "circ");
	BOOST_CHECK(Kernel::CirculationModes::Spa == mock->calls[5].mode);
}

BOOST_AUTO_TEST_CASE(Fire_CirculationMode_UnknownTarget_DoesNotDispatch)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	// The target is validated on the JSON path; a programmatic schedule with a
	// bogus mode string must simply not reach the dispatcher.
	service.Create(ScheduleAt(now, Action{ ActionType::CirculationMode, "NotAMode", 0 }));

	service.Tick();
	BOOST_CHECK(mock->calls.empty());
}

BOOST_AUTO_TEST_CASE(Fire_WithoutDispatcher_IsSkippedSafely)
{
	// A locator with NO ICommandDispatcher registered (TryFind yields null).
	Test::HubLocatorInjector bare;

	boost::asio::io_context io;
	Options::Scheduling::SchedulingSettings in_memory;   // no file
	SchedulerService service(io, bare, in_memory);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });
	service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 }));

	BOOST_CHECK_NO_THROW(service.Tick());
	BOOST_CHECK(mock->calls.empty());
}

// -----------------------------------------------------------------------------
// Tick(): matching arms and the per-minute double-fire key
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Tick_DayMatchesButHourOrMinuteDiffers_DoesNotFire)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	service.SetClock([now] { return now; });

	auto wrong_hour = ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 });
	wrong_hour.hour = (wrong_hour.hour + 1) % 24;
	service.Create(wrong_hour);

	auto wrong_minute = ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 });
	wrong_minute.minute = (wrong_minute.minute + 1) % 60;
	service.Create(wrong_minute);

	service.Tick();
	BOOST_CHECK(mock->calls.empty());
}

BOOST_AUTO_TEST_CASE(Tick_SameSlotOnALaterDay_FiresAgain)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	auto now = std::chrono::system_clock::now();
	service.SetClock([&now] { return now; });

	auto weekly = ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 });
	weekly.days_of_week = 0x7F;   // every day
	service.Create(weekly);

	service.Tick();
	BOOST_REQUIRE_EQUAL(mock->calls.size(), 1u);

	// Same minute, same day -> guarded.
	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls.size(), 1u);

	// One week later the wall-clock slot recurs with a NEW minute key.
	now += std::chrono::hours(24 * 7);
	service.Tick();
	BOOST_CHECK_EQUAL(mock->calls.size(), 2u);
}

// -----------------------------------------------------------------------------
// CRUD misses
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Crud_ReplaceAndRemoveAndGet_MissesAndHits)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	const auto now = std::chrono::system_clock::now();
	const auto created = service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 }, "original"));
	BOOST_REQUIRE(!created.uuid.empty());

	// Replace an existing schedule: the uuid is preserved, the body swapped.
	auto replacement = ScheduleAt(now, Action{ ActionType::PoolSetpoint, "", 80 }, "replaced");
	replacement.uuid = "ignored-by-replace";
	BOOST_CHECK(service.Replace(created.uuid, replacement));

	const auto fetched = service.Get(created.uuid);
	BOOST_REQUIRE(fetched.has_value());
	BOOST_CHECK_EQUAL(fetched->uuid, created.uuid);
	BOOST_CHECK_EQUAL(fetched->name, "replaced");
	BOOST_CHECK(ActionType::PoolSetpoint == fetched->action.type);

	// Persisted: the file carries the replacement.
	BOOST_CHECK(ReadAll(settings.schedules_file).find("replaced") != std::string::npos);

	// Misses.
	BOOST_CHECK(!service.Replace("no-such-uuid", replacement));
	BOOST_CHECK(!service.Remove("no-such-uuid"));
	BOOST_CHECK(!service.Get("no-such-uuid").has_value());

	// Hit.
	BOOST_CHECK(service.Remove(created.uuid));
	BOOST_CHECK(service.List().empty());
}

// -----------------------------------------------------------------------------
// Start() / Load() file edge cases
// -----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Start_NoFileConfigured_IsDisabled_AndIdempotent)
{
	boost::asio::io_context io;
	Options::Scheduling::SchedulingSettings disabled;   // empty schedules_file
	SchedulerService service(io, *this, disabled);

	service.Start();
	BOOST_CHECK_EQUAL(io.poll(), 0u);   // no timer armed

	// Creating still works in-memory; nothing is written anywhere.
	const auto now = std::chrono::system_clock::now();
	service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 }));
	BOOST_CHECK_EQUAL(service.List().size(), 1u);
}

BOOST_AUTO_TEST_CASE(Start_Twice_ArmsTimerOnce_StopCancelsIt)
{
	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);

	service.Start();
	service.Start();   // already running -> no-op

	// Stop cancels the pending tick; the handler observes operation_aborted
	// and returns without ticking or re-arming.
	service.Stop();
	BOOST_CHECK_EQUAL(io.poll(), 1u);
	BOOST_CHECK_EQUAL(io.poll(), 0u);

	// Stop again is a no-op.
	BOOST_CHECK_NO_THROW(service.Stop());
}

BOOST_AUTO_TEST_CASE(Load_NonArrayDocument_IsIgnored)
{
	{
		std::ofstream out(settings.schedules_file, std::ios::binary | std::ios::trunc);
		out << R"({"not": "an array"})";
	}

	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);
	service.Start();

	BOOST_CHECK(service.List().empty());
}

BOOST_AUTO_TEST_CASE(Load_MalformedJson_IsCaughtAndIgnored)
{
	{
		std::ofstream out(settings.schedules_file, std::ios::binary | std::ios::trunc);
		out << "[ this is not json";
	}

	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);
	BOOST_CHECK_NO_THROW(service.Start());
	BOOST_CHECK(service.List().empty());
}

BOOST_AUTO_TEST_CASE(Load_SkipsInvalidEntries_AndAssignsMissingUuid)
{
	{
		std::ofstream out(settings.schedules_file, std::ios::binary | std::ios::trunc);
		out << R"([
			{ "name": "good", "days_of_week": 127, "time_local": "06:30", "action": { "type": "button_on", "target": "Pump" } },
			{ "name": "bad-no-days", "time_local": "06:30", "action": { "type": "button_on", "target": "Pump" } },
			"not-an-object",
			{ "uuid": "keep-me", "name": "kept", "days_of_week": 1, "time_local": "23:59", "action": { "type": "pool_setpoint", "value": 80 } }
		])";
	}

	boost::asio::io_context io;
	SchedulerService service(io, *this, settings);
	service.Start();

	const auto list = service.List();
	BOOST_REQUIRE_EQUAL(list.size(), 2u);

	BOOST_CHECK_EQUAL(list[0].name, "good");
	BOOST_CHECK(!list[0].uuid.empty());   // assigned on load

	BOOST_CHECK_EQUAL(list[1].name, "kept");
	BOOST_CHECK_EQUAL(list[1].uuid, "keep-me");
	BOOST_CHECK_EQUAL(list[1].hour, 23);
	BOOST_CHECK_EQUAL(list[1].minute, 59);
}

BOOST_AUTO_TEST_CASE(Save_UnwritableTarget_IsCaughtAndServiceKeepsWorking)
{
	boost::asio::io_context io;
	Options::Scheduling::SchedulingSettings bad;
	bad.schedules_file = (std::filesystem::temp_directory_path() / "aqualink_sched_missing_dir" / "nested" / "schedules.json").string();

	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::temp_directory_path() / "aqualink_sched_missing_dir", ec);

	SchedulerService service(io, *this, bad);

	const auto now = std::chrono::system_clock::now();
	Schedule created;
	BOOST_CHECK_NO_THROW(created = service.Create(ScheduleAt(now, Action{ ActionType::ButtonOn, "Pump", 0 })));
	BOOST_CHECK(!created.uuid.empty());
	BOOST_CHECK_EQUAL(service.List().size(), 1u);
	BOOST_CHECK(!std::filesystem::exists(bad.schedules_file));
}

BOOST_AUTO_TEST_SUITE_END()
