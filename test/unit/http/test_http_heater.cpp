#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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
#include "http/webroute_equipment_heater.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/body_of_water_ids.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher: captures heater commands and returns a configurable result so the
	// test can prove invalid payloads are rejected BEFORE dispatch and that result codes map to
	// the right HTTP status.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<std::pair<Kernel::BodyOfWaterIds, bool>> heater_calls;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds body, bool enable) override { heater_calls.emplace_back(body, enable); return result_to_return; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct HeaterFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		HeaterFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body)
		{
			HTTP::WebRoute_Equipment_Heater route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(HTTP::EQUIPMENT_HEATER_ROUTE_URL);
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

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_Heater, HeaterFixture)

BOOST_AUTO_TEST_CASE(Post_SpaEnable_DispatchesAndReturnsOk)
{
	auto resp = Post(R"({"body": "spa", "enable": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->heater_calls.size());
	BOOST_CHECK(dispatcher->heater_calls.front().first == Kernel::BodyOfWaterIds::Spa);
	BOOST_CHECK(dispatcher->heater_calls.front().second == true);
}

BOOST_AUTO_TEST_CASE(Post_PoolDisable_Dispatches)
{
	auto resp = Post(R"({"body": "pool", "enable": false})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->heater_calls.size());
	BOOST_CHECK(dispatcher->heater_calls.front().first == Kernel::BodyOfWaterIds::Pool);
	BOOST_CHECK(dispatcher->heater_calls.front().second == false);
}

BOOST_AUTO_TEST_CASE(Post_Solar_MapsToSharedBody)
{
	auto resp = Post(R"({"body": "solar", "enable": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->heater_calls.size());
	BOOST_CHECK(dispatcher->heater_calls.front().first == Kernel::BodyOfWaterIds::Shared);
}

BOOST_AUTO_TEST_CASE(Post_UnknownBody_Returns400_NoDispatch)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"body": "lagoon", "enable": true})").result());
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Post_MissingBody_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"enable": true})").result());
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Post_MissingEnable_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"body": "spa"})").result());
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Post_EnableNotBoolean_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"body": "spa", "enable": 1})").result());
	BOOST_CHECK(dispatcher->heater_calls.empty());
}

BOOST_AUTO_TEST_CASE(Post_InvalidJson_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("{ not json").result());
}

BOOST_AUTO_TEST_CASE(Post_NonObjectBody_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("[1,2,3]").result());
}

BOOST_AUTO_TEST_CASE(Post_DispatchNoSerialAdapter_Returns503)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	auto resp = Post(R"({"body": "spa", "enable": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->heater_calls.size()); // value still reached the dispatcher
}

BOOST_AUTO_TEST_CASE(Get_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::get, "").result());
}

BOOST_AUTO_TEST_SUITE_END()
