#include <string>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "scheduling/schedule.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Scheduling;

BOOST_AUTO_TEST_SUITE(TestSuite_Schedule)

BOOST_AUTO_TEST_CASE(Schedule_ButtonAction_RoundTrip)
{
	Schedule s;
	s.uuid = "abc-123";
	s.name = "Morning pump";
	s.enabled = true;
	s.days_of_week = 0b0011111;   // Mon..Fri
	s.hour = 8;
	s.minute = 5;
	s.action.type = ActionType::ButtonOn;
	s.action.target = "Pool Pump";

	auto json = ToJson(s);
	BOOST_CHECK_EQUAL(json["time_local"], "08:05");
	BOOST_CHECK_EQUAL(json["action"]["type"], "button_on");
	BOOST_CHECK_EQUAL(json["action"]["target"], "Pool Pump");

	std::string error;
	auto parsed = FromJson(json, error);
	BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
	BOOST_CHECK_EQUAL(parsed->name, "Morning pump");
	BOOST_CHECK_EQUAL(parsed->days_of_week, 0b0011111);
	BOOST_CHECK_EQUAL(parsed->hour, 8);
	BOOST_CHECK_EQUAL(parsed->minute, 5);
	BOOST_CHECK(parsed->action.type == ActionType::ButtonOn);
	BOOST_CHECK_EQUAL(parsed->action.target, "Pool Pump");
}

BOOST_AUTO_TEST_CASE(Schedule_SetpointAction_RoundTrip)
{
	Schedule s;
	s.name = "Spa warm";
	s.days_of_week = 0b1000000;   // Sunday
	s.hour = 17;
	s.minute = 30;
	s.action.type = ActionType::SpaSetpoint;
	s.action.value = 38;

	auto json = ToJson(s);
	BOOST_CHECK_EQUAL(json["action"]["type"], "spa_setpoint");
	BOOST_CHECK_EQUAL(json["action"]["value"], 38);

	std::string error;
	auto parsed = FromJson(json, error);
	BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
	BOOST_CHECK(parsed->action.type == ActionType::SpaSetpoint);
	BOOST_CHECK_EQUAL(parsed->action.value, 38);
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsBadTime)
{
	nlohmann::json json = { { "days_of_week", 1 }, { "time_local", "25:99" }, { "action", { { "type", "button_toggle" }, { "target", "Pool Pump" } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsUnknownAction)
{
	nlohmann::json json = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", "explode" } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsSetpointOutOfRange)
{
	nlohmann::json json = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", "pool_setpoint" }, { "value", 200 } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsChlorinatorOutOfRange)
{
	nlohmann::json json = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", "chlorinator_percent" }, { "value", 150 } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsMissingDays)
{
	nlohmann::json json = { { "time_local", "08:00" }, { "action", { { "type", "button_toggle" }, { "target", "Pool Pump" } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

BOOST_AUTO_TEST_CASE(Schedule_RejectsButtonWithoutTarget)
{
	nlohmann::json json = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", "button_on" } } } };
	std::string error;
	BOOST_CHECK(!FromJson(json, error).has_value());
}

// Regression (found by the schedule-JSON fuzz harness): a present-but-wrong-typed
// OPTIONAL field must NOT throw. FromJson previously read "name"/"enabled" via
// nlohmann json.value(key, default), which throws type_error.302 when the key is
// present with a mismatched type — violating the documented "return nullopt / set
// error" contract and, since ParseScheduleBody (POST/PUT /api/schedules) parses with
// allow_exceptions=false and then calls FromJson with no try/catch, escaping as an
// uncaught exception on an untrusted request body. Wrong-typed optional fields are
// now ignored (matching ControllerScheduleFromJson), so an otherwise-valid body parses.
BOOST_AUTO_TEST_CASE(Schedule_WrongTypedOptionalFields_DoNotThrow)
{
	nlohmann::json json = {
		{ "name", 5 },            // number, not string
		{ "enabled", "yes" },     // string, not bool
		{ "days_of_week", 1 },
		{ "time_local", "08:00" },
		{ "action", { { "type", "button_toggle" }, { "target", "Pool Pump" } } },
	};
	std::string error;
	std::optional<Schedule> parsed;
	BOOST_REQUIRE_NO_THROW(parsed = FromJson(json, error));
	BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
	BOOST_CHECK(parsed->name.empty());   // wrong-typed name ignored
	BOOST_CHECK(parsed->enabled);        // wrong-typed enabled falls back to default
}

// Regression: a present-but-wrong-typed REQUIRED action field must be REJECTED with
// nullopt, never thrown (same root cause — action_json.value("type"/"target", ...)).
BOOST_AUTO_TEST_CASE(Schedule_WrongTypedActionFields_RejectedNotThrown)
{
	std::string error;
	std::optional<Schedule> parsed;

	// action.type as a number -> rejected (unknown action), not thrown.
	nlohmann::json bad_type = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", 7 } } } };
	BOOST_REQUIRE_NO_THROW(parsed = FromJson(bad_type, error));
	BOOST_CHECK(!parsed.has_value());

	// action.target as a number on a button action -> rejected, not thrown.
	nlohmann::json bad_target = { { "days_of_week", 1 }, { "time_local", "08:00" }, { "action", { { "type", "button_on" }, { "target", 9 } } } };
	BOOST_REQUIRE_NO_THROW(parsed = FromJson(bad_target, error));
	BOOST_CHECK(!parsed.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
