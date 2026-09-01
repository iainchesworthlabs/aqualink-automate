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

#include "http/server/server_types.h"
#include "http/webroute_equipment_circulation.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/circulation.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher: captures circulation-mode commands and returns a configurable result.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<Kernel::CirculationModes> modes;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t, AqualinkAutomate::Kernel::BodyOfWaterIds) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes mode) override { modes.push_back(mode); return result_to_return; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct CirculationFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		CirculationFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body)
		{
			HTTP::WebRoute_Equipment_Circulation route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(HTTP::EQUIPMENT_CIRCULATION_ROUTE_URL);
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

		HTTP::Response Post(const std::string& body) { return Send(boost::beast::http::verb::post, body); }

		std::shared_ptr<StubCommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_Circulation, CirculationFixture)

BOOST_AUTO_TEST_CASE(Post_Spa_DispatchesAndReturnsOk)
{
	auto resp = Post(R"({"mode": "spa"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->modes.size());
	BOOST_CHECK(dispatcher->modes.front() == Kernel::CirculationModes::Spa);
}

BOOST_AUTO_TEST_CASE(Post_Pool_Dispatches)
{
	auto resp = Post(R"({"mode": "pool"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->modes.size());
	BOOST_CHECK(dispatcher->modes.front() == Kernel::CirculationModes::Pool);
}

BOOST_AUTO_TEST_CASE(Post_Spillover_Dispatches)
{
	auto resp = Post(R"({"mode": "spillover"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->modes.size());
	BOOST_CHECK(dispatcher->modes.front() == Kernel::CirculationModes::Spillover);
}

BOOST_AUTO_TEST_CASE(Post_UnknownMode_Returns400_NoDispatch)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"mode": "drain"})").result());
	BOOST_CHECK(dispatcher->modes.empty());
}

BOOST_AUTO_TEST_CASE(Post_MissingMode_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"other": 1})").result());
	BOOST_CHECK(dispatcher->modes.empty());
}

BOOST_AUTO_TEST_CASE(Post_ModeNotString_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"mode": 2})").result());
	BOOST_CHECK(dispatcher->modes.empty());
}

BOOST_AUTO_TEST_CASE(Post_InvalidJson_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("{ not json").result());
}

BOOST_AUTO_TEST_CASE(Post_DispatchNoSerialAdapter_Returns503)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	auto resp = Post(R"({"mode": "spa"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->modes.size());
}

BOOST_AUTO_TEST_CASE(Get_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::get, "").result());
}

BOOST_AUTO_TEST_SUITE_END()
