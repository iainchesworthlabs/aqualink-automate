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
#include "http/webroute_equipment_chlorinator.h"
#include "interfaces/icommanddispatcher.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher with INDEPENDENT percentage/boost results so the "first failure
	// wins" ordering across the two fields can be pinned down.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult percentage_result{ CommandResult::Success };
		CommandResult boost_result{ CommandResult::Success };
		std::vector<std::uint8_t> percentages;
		std::vector<bool> boosts;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t percentage, AqualinkAutomate::Kernel::BodyOfWaterIds) override { percentages.push_back(percentage); return percentage_result; }
		CommandResult SetChlorinatorBoost(bool enable) override { boosts.push_back(enable); return boost_result; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct ChlorinatorBranchesFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		ChlorinatorBranchesFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
		}

		// Not registered by default so the "no dispatcher" 503 can be exercised.
		void RegisterDispatcher()
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body)
		{
			HTTP::WebRoute_Equipment_Chlorinator route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(verb);
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

		HTTP::Response Post(const std::string& body) { return Send(boost::beast::http::verb::post, body); }

		std::shared_ptr<StubCommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_ChlorinatorBranches, ChlorinatorBranchesFixture)

BOOST_AUTO_TEST_CASE(NonPost_Returns405)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::get, "").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::put, R"({"percentage": 50})").result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_NoDispatcher_Returns503)
{
	auto resp = Post(R"({"percentage": 50})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_NonObjectJson_Returns400)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("[50]").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("\"50\"").result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_BodyNotAString_Returns400_NoDispatch)
{
	RegisterDispatcher();
	auto resp = Post(R"({"percentage": 50, "body": 5})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->percentages.empty());
}

BOOST_AUTO_TEST_CASE(Post_PercentageIsRoundedToTheWireValue)
{
	RegisterDispatcher();
	auto resp = Post(R"({"percentage": 49.6})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->percentages.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->percentages.front()), 50);

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["percentage"]["value"].get<int>(), 50);
	BOOST_CHECK_EQUAL(body["percentage"]["body"].get<std::string>(), "Pool");
}

BOOST_AUTO_TEST_CASE(Post_BoundaryPercentages_AreAccepted)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, Post(R"({"percentage": 0})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, Post(R"({"percentage": 100})").result());
	BOOST_REQUIRE_EQUAL(dispatcher->percentages.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->percentages[0]), 0);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->percentages[1]), 100);
}

BOOST_AUTO_TEST_CASE(Post_PercentageAndBoost_BothDispatchedInOneRequest)
{
	RegisterDispatcher();
	auto resp = Post(R"({"percentage": 70, "body": "spa", "boost": false})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->percentages.size(), 1u);
	BOOST_REQUIRE_EQUAL(dispatcher->boosts.size(), 1u);
	BOOST_CHECK(!dispatcher->boosts.front());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["percentage"]["status"].get<std::string>(), "success");
	BOOST_CHECK_EQUAL(body["percentage"]["body"].get<std::string>(), "Spa");
	BOOST_CHECK_EQUAL(body["boost"]["status"].get<std::string>(), "success");
	BOOST_CHECK(!body["boost"]["value"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Post_DispatchInvalidValue_Returns400)
{
	RegisterDispatcher();
	dispatcher->percentage_result = Interfaces::ICommandDispatcher::CommandResult::InvalidValue;
	auto resp = Post(R"({"percentage": 50})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["percentage"]["status"].get<std::string>(), "error");
}

BOOST_AUTO_TEST_CASE(Post_DispatchNoSerialAdapter_Returns503)
{
	RegisterDispatcher();
	dispatcher->boost_result = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	auto resp = Post(R"({"boost": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["boost"]["status"].get<std::string>(), "error");
}

BOOST_AUTO_TEST_CASE(Post_DispatchUnknownEquipmentType_Returns422)
{
	RegisterDispatcher();
	dispatcher->percentage_result = Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType;
	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, Post(R"({"percentage": 50})").result());
}

BOOST_AUTO_TEST_CASE(Post_BothFieldsFail_FirstFailureDeterminesStatus)
{
	RegisterDispatcher();
	dispatcher->percentage_result = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	dispatcher->boost_result = Interfaces::ICommandDispatcher::CommandResult::Busy;

	auto resp = Post(R"({"percentage": 50, "boost": true})");
	// Percentage is handled first; its 503 must not be overwritten by the later Busy.
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["percentage"]["status"].get<std::string>(), "error");
	BOOST_CHECK_EQUAL(body["boost"]["status"].get<std::string>(), "error");
	BOOST_CHECK(!body.contains("code"));
}

BOOST_AUTO_TEST_CASE(Post_PercentageSucceedsBoostBusy_Returns409WithReason)
{
	RegisterDispatcher();
	dispatcher->boost_result = Interfaces::ICommandDispatcher::CommandResult::Busy;

	auto resp = Post(R"({"percentage": 50, "boost": true})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["percentage"]["status"].get<std::string>(), "success");
	BOOST_CHECK_EQUAL(body["boost"]["status"].get<std::string>(), "error");
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "chlorinator_busy");
	BOOST_CHECK(!body["error"].get<std::string>().empty());
}

BOOST_AUTO_TEST_SUITE_END()
