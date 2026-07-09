#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "scheduling/controller_schedule.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Scheduling;

BOOST_AUTO_TEST_SUITE(TestSuite_ControllerSchedule)

BOOST_AUTO_TEST_CASE(ControllerSchedule_Span_Serialises)
{
	ControllerSchedule c;
	c.id = "program-1";
	c.name = "Filter pump";
	c.target = "Filter Pump";
	c.enabled = true;
	c.days_of_week = 0x7f;   // every day
	c.on_hour = 8;
	c.on_minute = 0;
	c.off_hour = 12;
	c.off_minute = 30;

	auto json = ToJson(c);
	BOOST_CHECK_EQUAL(json["id"], "program-1");
	BOOST_CHECK_EQUAL(json["target"], "Filter Pump");
	BOOST_CHECK_EQUAL(json["enabled"], true);
	BOOST_CHECK_EQUAL(json["days_of_week"], 0x7f);
	BOOST_CHECK_EQUAL(json["on_local"], "08:00");
	BOOST_CHECK_EQUAL(json["off_local"], "12:30");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleStatus_StringMapping)
{
	BOOST_CHECK_EQUAL(ControllerScheduleStatusToString(ControllerScheduleStatus::Available), "available");
	BOOST_CHECK_EQUAL(ControllerScheduleStatusToString(ControllerScheduleStatus::PendingCapture), "pending_capture");
	BOOST_CHECK_EQUAL(ControllerScheduleStatusToString(ControllerScheduleStatus::Unsupported), "unsupported");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleStore_DefaultsToPendingAndEmpty)
{
	ControllerScheduleStore store;
	BOOST_CHECK(store.Status() == ControllerScheduleStatus::PendingCapture);
	BOOST_CHECK(store.List().empty());
}

BOOST_AUTO_TEST_CASE(ControllerScheduleStore_ReplaceSwapsSnapshot)
{
	ControllerScheduleStore store;

	std::vector<ControllerSchedule> programs;
	ControllerSchedule c;
	c.id = "program-1";
	c.target = "Filter Pump";
	programs.push_back(c);

	store.Replace(ControllerScheduleStatus::Available, std::move(programs));

	BOOST_CHECK(store.Status() == ControllerScheduleStatus::Available);
	BOOST_REQUIRE_EQUAL(store.List().size(), 1u);
	BOOST_CHECK_EQUAL(store.List().front().target, "Filter Pump");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleStatusToString_UnknownFallsBackToPendingCapture)
{
	// An out-of-range enum value hits the switch fall-through default.
	const auto bogus = static_cast<ControllerScheduleStatus>(0xff);
	BOOST_CHECK_EQUAL(ControllerScheduleStatusToString(bogus), "pending_capture");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_ValidBodyParses)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "on_local", "08:05" },
		{ "off_local", "12:30" },
		{ "name", "Morning run" },
		{ "group", "A" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(error.empty());
	BOOST_CHECK_EQUAL(result->target, "Filter Pump");
	BOOST_CHECK_EQUAL(static_cast<int>(result->days_of_week), 0x7f);
	BOOST_CHECK_EQUAL(result->on_hour, 8);
	BOOST_CHECK_EQUAL(result->on_minute, 5);
	BOOST_CHECK_EQUAL(result->off_hour, 12);
	BOOST_CHECK_EQUAL(result->off_minute, 30);
	BOOST_CHECK_EQUAL(result->name, "Morning run");
	BOOST_CHECK_EQUAL(result->group, "A");
	BOOST_CHECK(result->enabled);
	BOOST_CHECK(result->id.empty());
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_ValidBodyWithoutOptionalFields)
{
	nlohmann::json body{
		{ "target", "Spa" },
		{ "days_of_week", 0 },
		{ "on_local", "00:00" },
		{ "off_local", "23:59" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(error.empty());
	BOOST_CHECK_EQUAL(result->target, "Spa");
	BOOST_CHECK_EQUAL(static_cast<int>(result->days_of_week), 0);
	BOOST_CHECK_EQUAL(result->on_hour, 0);
	BOOST_CHECK_EQUAL(result->on_minute, 0);
	BOOST_CHECK_EQUAL(result->off_hour, 23);
	BOOST_CHECK_EQUAL(result->off_minute, 59);
	BOOST_CHECK(result->name.empty());
	BOOST_CHECK(result->group.empty());
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_NonObjectBodyRejected)
{
	std::string error;
	auto result = ControllerScheduleFromJson(nlohmann::json::array(), error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "body must be a JSON object");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MissingTargetRejected)
{
	nlohmann::json body{
		{ "days_of_week", 0x7f },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "target is required and must be a non-empty string");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_EmptyTargetRejected)
{
	nlohmann::json body{
		{ "target", "" },
		{ "days_of_week", 0x7f },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "target is required and must be a non-empty string");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MissingDaysOfWeekRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "days_of_week is required and must be an integer bitmask");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_NonIntegerDaysOfWeekRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", "seven" },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "days_of_week is required and must be an integer bitmask");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_DaysOfWeekOutOfRangeRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x80 },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "days_of_week must be in the range 0..127");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_NegativeDaysOfWeekRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", -1 },
		{ "on_local", "08:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "days_of_week must be in the range 0..127");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MissingOnLocalRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "on_local is required and must be a valid \"HH:MM\" time");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MalformedOnLocalRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "on_local", "8-00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "on_local is required and must be a valid \"HH:MM\" time");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_OutOfRangeOnLocalRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "on_local", "24:00" },
		{ "off_local", "12:30" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "on_local is required and must be a valid \"HH:MM\" time");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MissingOffLocalRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "on_local", "08:00" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "off_local is required and must be a valid \"HH:MM\" time");
}

BOOST_AUTO_TEST_CASE(ControllerScheduleFromJson_MalformedOffLocalRejected)
{
	nlohmann::json body{
		{ "target", "Filter Pump" },
		{ "days_of_week", 0x7f },
		{ "on_local", "08:00" },
		{ "off_local", "12:60" },
	};

	std::string error;
	auto result = ControllerScheduleFromJson(body, error);
	BOOST_CHECK(!result.has_value());
	BOOST_CHECK_EQUAL(error, "off_local is required and must be a valid \"HH:MM\" time");
}

BOOST_AUTO_TEST_SUITE_END()
