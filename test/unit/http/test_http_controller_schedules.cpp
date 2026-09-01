#include <algorithm>
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

#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_controller_schedules.h"
#include "interfaces/icommanddispatcher.h"
#include "scheduling/controller_schedule.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording dispatcher: captures controller-schedule writes and returns a configurable result.
	class StubDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<Scheduling::ControllerSchedule> created;
		std::vector<Scheduling::ControllerSchedule> deleted;
		std::vector<std::pair<Scheduling::ControllerSchedule, Scheduling::ControllerSchedule>> edited;

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
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule& p) override { created.push_back(p); return result_to_return; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule& p) override { deleted.push_back(p); return result_to_return; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) override { edited.emplace_back(existing, desired); return result_to_return; }
	};

	struct Fixture
	{
		std::shared_ptr<Scheduling::ControllerScheduleStore> store{ std::make_shared<Scheduling::ControllerScheduleStore>() };
		std::shared_ptr<StubDispatcher> dispatcher{ std::make_shared<StubDispatcher>() };

		Fixture()
		{
			Scheduling::ControllerSchedule s;
			s.id = "iaq-A-1"; s.target = "Pool Heat"; s.group = "A"; s.days_of_week = 0x7f;
			s.on_hour = 11; s.on_minute = 0; s.off_hour = 14; s.off_minute = 0;
			store->Replace(Scheduling::ControllerScheduleStatus::Available, { s }, "A");
		}

		static HTTP::Request MakeRequest(boost::beast::http::verb verb, const std::string& target, const std::string& body)
		{
			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(target);
			req.set(boost::beast::http::field::host, "localhost.localdomain");
			req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
			req.set(boost::beast::http::field::content_type, "application/json");
			req.body() = body;
			req.prepare_payload();
			return req;
		}

		static HTTP::Response Serialize(HTTP::Message msg)
		{
			boost::asio::io_context ioc;
			auto exec = ioc.get_executor();
			Test::MockBeastBasicStreamWithTimeout client_stream(exec);
			Test::MockBeastBasicStreamWithTimeout server_stream(exec);
			server_stream.connect(client_stream);

			boost::beast::error_code ec;
			boost::beast::write(server_stream, std::move(msg), ec);
			BOOST_REQUIRE_MESSAGE(!ec, "write response: " << ec.message());
			server_stream.close();
			ioc.poll();

			HTTP::Response resp;
			boost::beast::flat_buffer read_buffer;
			boost::beast::http::read(client_stream, read_buffer, resp, ec);
			BOOST_REQUIRE_MESSAGE(!ec || ec == boost::beast::http::error::end_of_stream, "read response: " << ec.message());
			return resp;
		}

		HTTP::Response Collection(boost::beast::http::verb verb, const std::string& body = "")
		{
			HTTP::WebRoute_ControllerSchedules route(store, dispatcher);
			return Serialize(route.OnRequest(MakeRequest(verb, "/api/controller/schedules", body)));
		}
		HTTP::Response Item(boost::beast::http::verb verb, const std::string& id, const std::string& body = "")
		{
			HTTP::WebRoute_ControllerSchedule route(store, dispatcher);
			return Serialize(route.OnRequest(MakeRequest(verb, "/api/controller/schedules/" + id, body)));
		}
	};

	constexpr auto k_get = boost::beast::http::verb::get;
	constexpr auto k_post = boost::beast::http::verb::post;
	constexpr auto k_put = boost::beast::http::verb::put;
	constexpr auto k_delete = boost::beast::http::verb::delete_;
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_ControllerSchedules, Fixture)

BOOST_AUTO_TEST_CASE(Get_ListsWithStatusAndActiveGroup)
{
	auto resp = Collection(k_get);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	auto body = nlohmann::json::parse(resp.body(), nullptr, false);
	BOOST_REQUIRE(!body.is_discarded());
	BOOST_CHECK_EQUAL(body["status"].get<std::string>(), "available");
	BOOST_CHECK_EQUAL(body["active_group"].get<std::string>(), "A");
	BOOST_REQUIRE_EQUAL(body["schedules"].size(), 1u);
	BOOST_CHECK_EQUAL(body["schedules"][0]["target"].get<std::string>(), "Pool Heat");
}

BOOST_AUTO_TEST_CASE(Post_Valid_QueuesCreate)
{
	auto resp = Collection(k_post, R"({"target":"Filter Pump","days_of_week":127,"on_local":"09:00","off_local":"17:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->created.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->created[0].target, "Filter Pump");
	BOOST_CHECK_EQUAL(dispatcher->created[0].on_hour, 9);
	BOOST_CHECK_EQUAL(dispatcher->created[0].off_hour, 17);
	BOOST_CHECK_EQUAL(dispatcher->created[0].days_of_week, 0x7f);
}

BOOST_AUTO_TEST_CASE(Post_MissingTarget_Returns400_NoDispatch)
{
	auto resp = Collection(k_post, R"({"days_of_week":127,"on_local":"09:00","off_local":"17:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_CASE(Post_NotRepresentableDays_Returns400_WithBlockers)
{
	// Mon+Wed+Fri (0x15) cannot be represented on the controller.
	auto resp = Collection(k_post, R"({"target":"Filter Pump","days_of_week":21,"on_local":"09:00","off_local":"17:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->created.empty());
	auto body = nlohmann::json::parse(resp.body(), nullptr, false);
	BOOST_REQUIRE(!body.is_discarded());
	BOOST_REQUIRE(body.contains("blockers"));
	const auto blockers = body["blockers"];
	BOOST_CHECK(std::find(blockers.begin(), blockers.end(), "day_selection_not_expressible") != blockers.end());
}

BOOST_AUTO_TEST_CASE(Post_BadTime_Returns400)
{
	auto resp = Collection(k_post, R"({"target":"Filter Pump","days_of_week":127,"on_local":"9 o'clock","off_local":"17:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_CASE(Post_DispatchNoWriter_Returns503)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	auto resp = Collection(k_post, R"({"target":"Filter Pump","days_of_week":127,"on_local":"09:00","off_local":"17:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->created.size(), 1u);   // the value still reached the dispatcher
}

BOOST_AUTO_TEST_CASE(Put_Collection_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, Collection(k_put, "{}").result());
}

BOOST_AUTO_TEST_CASE(Delete_KnownId_QueuesDelete)
{
	auto resp = Item(k_delete, "iaq-A-1");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->deleted.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->deleted[0].target, "Pool Heat");
}

BOOST_AUTO_TEST_CASE(Delete_UnknownId_Returns404)
{
	auto resp = Item(k_delete, "iaq-A-999");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(dispatcher->deleted.empty());
}

BOOST_AUTO_TEST_CASE(Put_KnownId_Valid_QueuesEdit)
{
	// The fixture's existing program is iaq-A-1 (Pool Heat, All days, 11:00->14:00).
	auto resp = Item(k_put, "iaq-A-1", R"({"target":"Pool Heat","days_of_week":127,"on_local":"11:00","off_local":"15:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->edited.size(), 1u);
	// existing resolved from the store...
	BOOST_CHECK_EQUAL(dispatcher->edited[0].first.id, "iaq-A-1");
	BOOST_CHECK_EQUAL(dispatcher->edited[0].first.target, "Pool Heat");
	BOOST_CHECK_EQUAL(dispatcher->edited[0].first.off_hour, 14);
	// ...desired parsed from the body.
	BOOST_CHECK_EQUAL(dispatcher->edited[0].second.target, "Pool Heat");
	BOOST_CHECK_EQUAL(dispatcher->edited[0].second.off_hour, 15);
	BOOST_CHECK_EQUAL(dispatcher->edited[0].second.days_of_week, 0x7f);
}

BOOST_AUTO_TEST_CASE(Put_UnknownId_Returns404_NoDispatch)
{
	auto resp = Item(k_put, "iaq-A-999", R"({"target":"Pool Heat","days_of_week":127,"on_local":"11:00","off_local":"15:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(dispatcher->edited.empty());
}

BOOST_AUTO_TEST_CASE(Put_NotRepresentableDays_Returns400_WithBlockers_NoDispatch)
{
	// Mon+Wed+Fri (0x15) is not controller-representable.
	auto resp = Item(k_put, "iaq-A-1", R"({"target":"Pool Heat","days_of_week":21,"on_local":"11:00","off_local":"15:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->edited.empty());
	auto body = nlohmann::json::parse(resp.body(), nullptr, false);
	BOOST_REQUIRE(!body.is_discarded());
	BOOST_REQUIRE(body.contains("blockers"));
	const auto blockers = body["blockers"];
	BOOST_CHECK(std::find(blockers.begin(), blockers.end(), "day_selection_not_expressible") != blockers.end());
}

BOOST_AUTO_TEST_CASE(Put_BadBody_Returns400_NoDispatch)
{
	auto resp = Item(k_put, "iaq-A-1", R"({"target":"Pool Heat","days_of_week":127,"on_local":"11 o'clock","off_local":"15:00"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK(dispatcher->edited.empty());
}

BOOST_AUTO_TEST_SUITE_END()
