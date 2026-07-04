#include <cstdint>
#include <memory>
#include <optional>
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
#include "http/webroute_equipment_chlorinator.h"
#include "interfaces/icommanddispatcher.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher: captures chlorinator commands and returns a configurable
	// result so the test can prove invalid payloads are rejected BEFORE dispatch.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<std::uint8_t> percentages;
		std::vector<bool> boosts;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t percentage) override { percentages.push_back(percentage); return result_to_return; }
		CommandResult SetChlorinatorBoost(bool enable) override { boosts.push_back(enable); return result_to_return; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct ChlorinatorFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		ChlorinatorFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		HTTP::Response Post(const std::string& body)
		{
			HTTP::WebRoute_Equipment_Chlorinator route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::post);
			req.target(HTTP::EQUIPMENT_CHLORINATOR_ROUTE_URL);
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

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_Chlorinator, ChlorinatorFixture)

BOOST_AUTO_TEST_CASE(Post_ValidPercentage_DispatchesAndReturnsOk)
{
	auto resp = Post(R"({"percentage": 60})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->percentages.size());
	BOOST_CHECK_EQUAL(60, static_cast<int>(dispatcher->percentages.front()));
}

BOOST_AUTO_TEST_CASE(Post_BoostBoolean_Dispatches)
{
	auto resp = Post(R"({"boost": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->boosts.size());
	BOOST_CHECK(dispatcher->boosts.front());
}

BOOST_AUTO_TEST_CASE(Post_PercentageOutOfRange_Returns400_NoDispatch)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"percentage": 101})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"percentage": -1})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"percentage": 1e9})").result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_PercentageNotNumber_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"percentage": "high"})").result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_BoostNotBoolean_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"boost": 1})").result());
	BOOST_CHECK(dispatcher->boosts.empty());
}

BOOST_AUTO_TEST_CASE(Post_DispatchDeviceNotFound_Returns503)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound;
	auto resp = Post(R"({"percentage": 50})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_REQUIRE_EQUAL(1u, dispatcher->percentages.size()); // value still reached the dispatcher
}

BOOST_AUTO_TEST_CASE(Post_MissingFields_NoOpOk)
{
	auto resp = Post(R"({"other": 1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK(dispatcher->percentages.empty());
	BOOST_CHECK(dispatcher->boosts.empty());
}

BOOST_AUTO_TEST_CASE(Post_InvalidJson_Returns400)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("{ not json").result());
}

BOOST_AUTO_TEST_SUITE_END()
