#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_equipment_setpoints.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/data_hub.h"
#include "kernel/temperature.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher with INDEPENDENT pool/spa results so the "worst result wins" logic
	// across the two keys can be pinned down.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult pool_result{ CommandResult::Success };
		CommandResult spa_result{ CommandResult::Success };
		std::vector<uint8_t> pool_values;
		std::vector<uint8_t> spa_values;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(uint8_t temperature) override { pool_values.push_back(temperature); return pool_result; }
		CommandResult SetSpaSetpoint(uint8_t temperature) override { spa_values.push_back(temperature); return spa_result; }
		CommandResult SetChlorinatorPercentage(std::uint8_t, AqualinkAutomate::Kernel::BodyOfWaterIds) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct SetpointsBranchesFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		SetpointsBranchesFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
		}

		// Not registered by default so the "no dispatcher" 503 can be exercised; every command
		// test calls this first.
		void RegisterDispatcher()
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		std::shared_ptr<Kernel::DataHub> DataHub() { return Find<Kernel::DataHub>(); }

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body)
		{
			HTTP::WebRoute_Equipment_Setpoints route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(HTTP::EQUIPMENT_SETPOINTS_ROUTE_URL);
			req.set(boost::beast::http::field::host, "localhost.localdomain");
			req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
			req.set(boost::beast::http::field::content_type, "application/json");
			req.body() = body;
			req.prepare_payload();

			HTTP::Message msg = route.OnRequest(req);

			boost::asio::io_context ioc;
			auto exec = ioc.get_executor();

			Test::MockBeastBasicStreamWithTimeout client_stream(exec);
			Test::MockBeastBasicStreamWithTimeout server_stream(exec);
			server_stream.connect(client_stream);

			boost::beast::error_code ec;
			boost::beast::write(server_stream, std::move(msg), ec);
			BOOST_REQUIRE_MESSAGE(!ec, "Failed to write response: " << ec.message());
			server_stream.close();
			ioc.poll();

			HTTP::Response resp;
			boost::beast::flat_buffer read_buffer;
			boost::beast::http::read(client_stream, read_buffer, resp, ec);
			BOOST_REQUIRE_MESSAGE(!ec || ec == boost::beast::http::error::end_of_stream, "Failed to read response: " << ec.message());
			return resp;
		}

		HTTP::Response Get() { return Send(boost::beast::http::verb::get, ""); }
		HTTP::Response Post(const std::string& body) { return Send(boost::beast::http::verb::post, body); }

		std::shared_ptr<StubCommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_SetpointsBranches, SetpointsBranchesFixture)

//-----------------------------------------------------------------------------
// GET
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Get_NothingReportedYet_EveryFieldIsNull)
{
	auto resp = Get();
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK(body["pool_setpoint"].is_null());
	BOOST_CHECK(body["pool_setpoint_2"].is_null());
	BOOST_CHECK(body["pool_heater_2_enabled"].is_null());
	BOOST_CHECK(body["spa_setpoint"].is_null());
}

BOOST_AUTO_TEST_CASE(Get_ReportedSetpoints_AreSerialisedInCelsius)
{
	DataHub()->PoolTempSetpoint(Kernel::Temperature::ConvertToTemperatureInCelsius(28.0));
	DataHub()->PoolTempSetpoint2(Kernel::Temperature::ConvertToTemperatureInCelsius(24.0));
	DataHub()->PoolHeater2Enabled(false);
	DataHub()->SpaTempSetpoint(Kernel::Temperature::ConvertToTemperatureInCelsius(38.0));

	const auto body = nlohmann::json::parse(Get().body());
	BOOST_CHECK_CLOSE(body["pool_setpoint"]["celsius"].get<double>(), 28.0, 0.01);
	BOOST_CHECK_CLOSE(body["pool_setpoint_2"]["celsius"].get<double>(), 24.0, 0.01);
	BOOST_REQUIRE(body["pool_heater_2_enabled"].is_boolean());
	BOOST_CHECK(!body["pool_heater_2_enabled"].get<bool>());
	BOOST_CHECK_CLOSE(body["spa_setpoint"]["celsius"].get<double>(), 38.0, 0.01);
	// The primary setpoints carry freshness metadata; the read-only TEMP2 does not.
	BOOST_CHECK(body["pool_setpoint"].contains("stale"));
	BOOST_CHECK(!body["pool_setpoint_2"].contains("stale"));
}

//-----------------------------------------------------------------------------
// Method / dispatcher guards
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Delete_Returns405)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::delete_, "").result());
}

BOOST_AUTO_TEST_CASE(Post_NoDispatcher_Returns503)
{
	auto resp = Post(R"({"pool": 28.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "dispatcher_unavailable");
	BOOST_CHECK(dispatcher->pool_values.empty());
}

//-----------------------------------------------------------------------------
// POST -- the spa key and the unit-conversion branch
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Post_SpaNotANumber_NamesTheOffendingTarget)
{
	RegisterDispatcher();
	auto resp = Post(R"({"spa": "warm"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "setpoint_not_a_number");
	BOOST_CHECK_EQUAL(body["params"]["target"].get<std::string>(), "spa");
	BOOST_CHECK(dispatcher->spa_values.empty());
}

BOOST_AUTO_TEST_CASE(Post_SpaOutOfRange_ReportsBounds)
{
	RegisterDispatcher();
	auto resp = Post(R"({"spa": 60.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "setpoint_out_of_range");
	BOOST_CHECK_EQUAL(body["params"]["target"].get<std::string>(), "spa");
	BOOST_CHECK_CLOSE(body["params"]["min"].get<double>(), -10.0, 0.01);
	BOOST_CHECK_CLOSE(body["params"]["max"].get<double>(), 50.0, 0.01);
	BOOST_CHECK(dispatcher->spa_values.empty());
}

BOOST_AUTO_TEST_CASE(Post_CelsiusSystem_SendsCelsiusOnTheWire)
{
	RegisterDispatcher();
	DataHub()->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);

	auto resp = Post(R"({"pool": 27.6, "spa": 38.4})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->pool_values.size(), 1u);
	BOOST_REQUIRE_EQUAL(dispatcher->spa_values.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->pool_values.front()), 28); // rounded, not converted
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->spa_values.front()), 38);

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["pool"]["status"].get<std::string>(), "success");
	BOOST_CHECK_CLOSE(body["pool"]["celsius"].get<double>(), 27.6, 0.01);
	BOOST_CHECK_EQUAL(body["spa"]["status"].get<std::string>(), "success");
}

BOOST_AUTO_TEST_CASE(Post_FahrenheitSystem_ConvertsSpaSetpoint)
{
	RegisterDispatcher();
	auto resp = Post(R"({"spa": 38.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->spa_values.size(), 1u);
	// 38C -> 100.4F -> 100.
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->spa_values.front()), 100);
}

BOOST_AUTO_TEST_CASE(Post_BothKeysFail_FirstFailureDeterminesStatus)
{
	RegisterDispatcher();
	dispatcher->pool_result = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	dispatcher->spa_result = Interfaces::ICommandDispatcher::CommandResult::Busy;

	auto resp = Post(R"({"pool": 28.0, "spa": 38.0})");
	// Pool is dispatched first and its failure (503) is what the response reports; the later
	// Busy result must not overwrite it.
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->pool_values.size(), 1u);
	BOOST_REQUIRE_EQUAL(dispatcher->spa_values.size(), 1u);

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["pool"]["status"].get<std::string>(), "error");
	BOOST_CHECK_EQUAL(body["spa"]["status"].get<std::string>(), "error");
	BOOST_CHECK(!body.contains("code"));
}

BOOST_AUTO_TEST_CASE(Post_PoolSucceedsSpaBusy_Returns409WithReason)
{
	RegisterDispatcher();
	dispatcher->spa_result = Interfaces::ICommandDispatcher::CommandResult::Busy;

	auto resp = Post(R"({"pool": 28.0, "spa": 38.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["pool"]["status"].get<std::string>(), "success");
	BOOST_CHECK_EQUAL(body["spa"]["status"].get<std::string>(), "error");
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "setpoint_busy");
}

BOOST_AUTO_TEST_CASE(Post_SpaInvalidValue_Returns400)
{
	RegisterDispatcher();
	dispatcher->spa_result = Interfaces::ICommandDispatcher::CommandResult::InvalidValue;

	auto resp = Post(R"({"spa": 38.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["spa"]["status"].get<std::string>(), "error");
}

BOOST_AUTO_TEST_CASE(Post_UnknownEquipmentType_Returns422)
{
	RegisterDispatcher();
	dispatcher->pool_result = Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType;

	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, Post(R"({"pool": 28.0})").result());
}

BOOST_AUTO_TEST_CASE(Post_LowerBoundaryValues_AreAccepted)
{
	RegisterDispatcher();
	DataHub()->SystemTemperatureUnits(Kernel::TemperatureUnits::Celsius);

	// -10C is the documented floor; the wire clamp keeps the uint8_t cast defined.
	auto resp = Post(R"({"pool": -10.0})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->pool_values.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->pool_values.front()), 0);
}

BOOST_AUTO_TEST_SUITE_END()
