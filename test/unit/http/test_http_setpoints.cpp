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
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Minimal recording ICommandDispatcher: every setpoint dispatch records the wire value and
	// returns a configurable result so the test can assert that an out-of-range/invalid payload is
	// rejected BEFORE it reaches the dispatcher.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<uint8_t> pool_values;
		std::vector<uint8_t> spa_values;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(uint8_t temperature) override { pool_values.push_back(temperature); return result_to_return; }
		CommandResult SetSpaSetpoint(uint8_t temperature) override { spa_values.push_back(temperature); return result_to_return; }
		CommandResult SetChlorinatorPercentage(uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct SetpointsFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		SetpointsFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
			// The route resolves ICommandDispatcher via TryFind, so it must be registered as the
			// interface type (the WrapperImpl is keyed on the registered shared_ptr's type).
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		// Serialise the route's message_generator response and parse the resulting wire bytes back
		// into a Response so the status code and body can be inspected (mirrors the GET test helper).
		HTTP::Response Post(const std::string& body)
		{
			HTTP::WebRoute_Equipment_Setpoints route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::post);
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

		std::shared_ptr<StubCommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_Setpoints, SetpointsFixture)

// =============================================================================
// Regression for WU-HTTP-EQUIPMENT-SETPOINTS
// =============================================================================

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_ValidPool_DispatchesAndReturnsOk)
{
	auto resp = Post(R"({"pool": 28.0})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->pool_values.size());
	// Default units are Fahrenheit: 28C -> ~82F.
	BOOST_CHECK_EQUAL(82, static_cast<int>(dispatcher->pool_values.front()));
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_NonNumericSetpoint_Returns400)
{
	// Well-formed JSON, but the setpoint field is a string -> must be 400, never 500.
	auto resp = Post(R"({"pool": "hot"})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	// The dispatcher must NOT have been called with a bogus value.
	BOOST_CHECK(dispatcher->pool_values.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_NullSetpoint_Returns400)
{
	auto resp = Post(R"({"spa": null})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->spa_values.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_OutOfRangeHuge_Returns400_NoUB)
{
	// An attacker-controlled enormous value would overflow the uint8_t cast (UB) if not guarded.
	auto resp = Post(R"({"pool": 1e9})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->pool_values.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_OutOfRangeNegative_Returns400)
{
	auto resp = Post(R"({"spa": -500.0})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->spa_values.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_DispatchFailure_ReturnsNon2xx)
{
	// When the dispatcher reports a failure, the HTTP status must be non-2xx so the UI's resp.ok
	// is authoritative rather than always-true.
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;

	auto resp = Post(R"({"pool": 28.0})");

	BOOST_CHECK(boost::beast::http::to_status_class(resp.result()) != boost::beast::http::status_class::successful);
	BOOST_REQUIRE_EQUAL(1u, dispatcher->pool_values.size());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_MissingFields_ReturnsOk)
{
	// A payload with neither pool nor spa is a no-op success.
	auto resp = Post(R"({"other": 1})");

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK(dispatcher->pool_values.empty());
	BOOST_CHECK(dispatcher->spa_values.empty());
}

BOOST_AUTO_TEST_CASE(Test_Setpoints_Post_InvalidJson_Returns400)
{
	auto resp = Post("{ this is not json");

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
}

BOOST_AUTO_TEST_SUITE_END()
