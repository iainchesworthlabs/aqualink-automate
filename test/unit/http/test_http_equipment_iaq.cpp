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
#include "http/webroute_equipment_iaq.h"
#include "interfaces/icommanddispatcher.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher: captures IAQ page-button selections and returns a configurable
	// result so every CommandResult -> HTTP status mapping can be driven.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<std::uint8_t> selected_buttons;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t, AqualinkAutomate::Kernel::BodyOfWaterIds) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t button_index) override { selected_buttons.push_back(button_index); return result_to_return; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct IAQFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		IAQFixture()
			: dispatcher(std::make_shared<StubCommandDispatcher>())
		{
		}

		// The route resolves the dispatcher in its constructor via TryFind, so a test wanting the
		// "no dispatcher" 503 simply never calls this.
		void RegisterDispatcher()
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body)
		{
			HTTP::WebRoute_Equipment_IAQ route(*this);

			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(HTTP::EQUIPMENT_IAQ_ROUTE_URL);
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

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_EquipmentIAQ, IAQFixture)

BOOST_AUTO_TEST_CASE(NonPost_Returns405)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::get, "").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Send(boost::beast::http::verb::put, R"({"select_button":1})").result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
}

BOOST_AUTO_TEST_CASE(Post_NoDispatcher_Returns503)
{
	auto resp = Post(R"({"select_button":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
}

BOOST_AUTO_TEST_CASE(Post_NonObjectBody_Returns400)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("{ not json").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("[1, 2]").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post("42").result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
}

BOOST_AUTO_TEST_CASE(Post_SelectButtonNotInteger_Returns400)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"select_button":"2"})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"select_button":1.5})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"select_button":null})").result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
}

BOOST_AUTO_TEST_CASE(Post_SelectButtonOutOfRange_Returns400)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"select_button":-1})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, Post(R"({"select_button":256})").result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
}

BOOST_AUTO_TEST_CASE(Post_SelectButtonBoundaries_AreAccepted)
{
	RegisterDispatcher();
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, Post(R"({"select_button":0})").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, Post(R"({"select_button":255})").result());
	BOOST_REQUIRE_EQUAL(dispatcher->selected_buttons.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->selected_buttons[0]), 0);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->selected_buttons[1]), 255);
}

BOOST_AUTO_TEST_CASE(Post_Success_DispatchesAndEchoesValue)
{
	RegisterDispatcher();
	auto resp = Post(R"({"select_button":7})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->selected_buttons.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(dispatcher->selected_buttons.front()), 7);

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["select_button"]["status"].get<std::string>(), "success");
	BOOST_CHECK_EQUAL(body["select_button"]["value"].get<int>(), 7);
	BOOST_CHECK(!body.contains("error"));
	BOOST_CHECK(!body.contains("code"));
}

BOOST_AUTO_TEST_CASE(Post_NoRecognisedField_IsANoOpOk)
{
	RegisterDispatcher();
	auto resp = Post(R"({"other":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK(dispatcher->selected_buttons.empty());
	BOOST_CHECK(nlohmann::json::parse(resp.body()).empty());
}

BOOST_AUTO_TEST_CASE(Post_DispatchInvalidValue_Returns400)
{
	RegisterDispatcher();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::InvalidValue;
	auto resp = Post(R"({"select_button":3})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["select_button"]["status"].get<std::string>(), "error");
}

BOOST_AUTO_TEST_CASE(Post_DispatchDeviceNotFound_Returns503)
{
	RegisterDispatcher();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound;
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, Post(R"({"select_button":3})").result());
}

BOOST_AUTO_TEST_CASE(Post_DispatchNoSerialAdapter_Returns503)
{
	RegisterDispatcher();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, Post(R"({"select_button":3})").result());
}

BOOST_AUTO_TEST_CASE(Post_DispatchUnknownEquipmentType_Returns422)
{
	RegisterDispatcher();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType;
	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, Post(R"({"select_button":3})").result());
}

BOOST_AUTO_TEST_CASE(Post_DispatchBusy_Returns409WithReason)
{
	RegisterDispatcher();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::Busy;
	auto resp = Post(R"({"select_button":3})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["select_button"]["status"].get<std::string>(), "error");
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "iaq_busy");
	BOOST_CHECK(!body["error"].get<std::string>().empty());
}

BOOST_AUTO_TEST_SUITE_END()
