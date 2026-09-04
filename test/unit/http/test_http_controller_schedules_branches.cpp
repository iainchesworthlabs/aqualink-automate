#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_controller_schedules.h"
#include "interfaces/icommanddispatcher.h"
#include "scheduling/controller_schedule.h"

using namespace AqualinkAutomate;

//=============================================================================
// Branch coverage for the controller-schedule routes that the happy-path suite
// (test_http_controller_schedules.cpp) does not reach: the no-store 503s, the
// no-dispatcher 503s, the non-object body 400s, the item route's missing-id and
// method guards, and the dispatcher's "rejected" (busy / invalid) acks.
//=============================================================================

namespace
{
	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;
	constexpr auto PUT = boost::beast::http::verb::put;
	constexpr auto DELETE_ = boost::beast::http::verb::delete_;

	constexpr const char* VALID_PROGRAM{ R"({"target":"Filter Pump","days_of_week":127,"on_local":"09:00","off_local":"17:00"})" };

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

	HTTP::Request MakeRequest(boost::beast::http::verb verb, const std::string& target, const std::string& body = {})
	{
		HTTP::Request req;
		req.version(11);
		req.method(verb);
		req.target(target);
		req.set(boost::beast::http::field::host, "localhost.localdomain");

		if (!body.empty())
		{
			req.set(boost::beast::http::field::content_type, "application/json");
			req.body() = body;
			req.prepare_payload();
		}

		return req;
	}

	nlohmann::json BodyOf(const HTTP::Response& resp)
	{
		return nlohmann::json::parse(resp.body(), nullptr, false);
	}

	struct BranchesFixture
	{
		std::shared_ptr<Scheduling::ControllerScheduleStore> store{ std::make_shared<Scheduling::ControllerScheduleStore>() };
		std::shared_ptr<StubDispatcher> dispatcher{ std::make_shared<StubDispatcher>() };

		BranchesFixture()
		{
			Scheduling::ControllerSchedule s;
			s.id = "iaq-A-1";
			s.target = "Pool Heat";
			s.group = "A";
			s.days_of_week = 0x7f;
			s.on_hour = 11;
			s.on_minute = 0;
			s.off_hour = 14;
			s.off_minute = 0;
			store->Replace(Scheduling::ControllerScheduleStatus::Available, { s }, "A");
		}
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_ControllerSchedulesBranches, BranchesFixture)

//-----------------------------------------------------------------------------
// No read store at all: both routes answer 503 before looking at the request
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(NoStore_CollectionAndItem_Return503)
{
	HTTP::WebRoute_ControllerSchedules collection{ nullptr, dispatcher };
	BOOST_CHECK(boost::beast::http::status::service_unavailable == collection.OnRequest(MakeRequest(GET, "/api/controller/schedules")).result());
	BOOST_CHECK(boost::beast::http::status::service_unavailable == collection.OnRequest(MakeRequest(POST, "/api/controller/schedules", VALID_PROGRAM)).result());

	HTTP::WebRoute_ControllerSchedule item{ nullptr, dispatcher };
	BOOST_CHECK(boost::beast::http::status::service_unavailable == item.OnRequest(MakeRequest(DELETE_, "/api/controller/schedules/iaq-A-1")).result());

	BOOST_CHECK(dispatcher->created.empty());
	BOOST_CHECK(dispatcher->deleted.empty());
}

//-----------------------------------------------------------------------------
// Collection route
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Collection_Post_NoDispatcher_Returns503)
{
	HTTP::WebRoute_ControllerSchedules route{ store };   // default: no dispatcher

	// GET still works from the read store alone...
	const auto listed = route.OnRequest(MakeRequest(GET, "/api/controller/schedules"));
	BOOST_CHECK(boost::beast::http::status::ok == listed.result());
	BOOST_CHECK_EQUAL(BodyOf(listed)["schedules"].size(), 1u);

	// ...but a write has nowhere to go.
	const auto resp = route.OnRequest(MakeRequest(POST, "/api/controller/schedules", VALID_PROGRAM));
	BOOST_CHECK(boost::beast::http::status::service_unavailable == resp.result());
	BOOST_CHECK_EQUAL(resp.body(), "Command dispatcher not available");
}

BOOST_AUTO_TEST_CASE(Collection_Post_NonObjectBodies_Return400)
{
	HTTP::WebRoute_ControllerSchedules route{ store, dispatcher };

	for (const auto* body : { "[]", "not json", "42", "\"text\"" })
	{
		const auto resp = route.OnRequest(MakeRequest(POST, "/api/controller/schedules", body));
		BOOST_CHECK_MESSAGE(boost::beast::http::status::bad_request == resp.result(), body);
		BOOST_CHECK_EQUAL(resp.body(), "request body must be a JSON object");
	}

	// An empty body is not an object either.
	BOOST_CHECK(boost::beast::http::status::bad_request == route.OnRequest(MakeRequest(POST, "/api/controller/schedules")).result());

	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_CASE(Collection_Post_DispatcherBusy_Returns409Rejected)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::Busy;

	HTTP::WebRoute_ControllerSchedules route{ store, dispatcher };
	const auto resp = route.OnRequest(MakeRequest(POST, "/api/controller/schedules", VALID_PROGRAM));

	BOOST_CHECK(boost::beast::http::status::conflict == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK_EQUAL(body.value("status", ""), "rejected");
	BOOST_REQUIRE(body.contains("schedule"));
	BOOST_CHECK_EQUAL(body["schedule"].value("target", ""), "Filter Pump");
	BOOST_REQUIRE_EQUAL(dispatcher->created.size(), 1u);
}

BOOST_AUTO_TEST_CASE(Collection_Post_InvalidValue_Returns400Rejected)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::InvalidValue;

	HTTP::WebRoute_ControllerSchedules route{ store, dispatcher };
	const auto resp = route.OnRequest(MakeRequest(POST, "/api/controller/schedules", VALID_PROGRAM));

	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("status", ""), "rejected");
}

BOOST_AUTO_TEST_CASE(Collection_Post_Success_AcksQueued)
{
	HTTP::WebRoute_ControllerSchedules route{ store, dispatcher };
	const auto resp = route.OnRequest(MakeRequest(POST, "/api/controller/schedules", R"({"target":"Filter Pump","days_of_week":127,"on_local":"09:30","off_local":"17:45","name":"Daily","group":"B"})"));

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK_EQUAL(body.value("status", ""), "queued");
	BOOST_CHECK_EQUAL(body["schedule"].value("target", ""), "Filter Pump");
	BOOST_CHECK_EQUAL(body["schedule"].value("on_local", ""), "09:30");
	BOOST_CHECK_EQUAL(body["schedule"].value("off_local", ""), "17:45");
}

BOOST_AUTO_TEST_CASE(Collection_DeleteVerb_Returns405)
{
	HTTP::WebRoute_ControllerSchedules route{ store, dispatcher };
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == route.OnRequest(MakeRequest(DELETE_, "/api/controller/schedules")).result());
}

//-----------------------------------------------------------------------------
// Item route
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Item_MissingProgramId_Returns400)
{
	HTTP::WebRoute_ControllerSchedule route{ store, dispatcher };

	// A trailing slash leaves an EMPTY last segment: no id to resolve.
	const auto resp = route.OnRequest(MakeRequest(DELETE_, "/api/controller/schedules/"));
	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
	BOOST_CHECK_EQUAL(resp.body(), "missing program id");

	// The bare root has no segments at all.
	BOOST_CHECK(boost::beast::http::status::bad_request == route.OnRequest(MakeRequest(DELETE_, "/")).result());

	BOOST_CHECK(dispatcher->deleted.empty());
}

BOOST_AUTO_TEST_CASE(Item_GetAndPost_Return405)
{
	HTTP::WebRoute_ControllerSchedule route{ store, dispatcher };

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == route.OnRequest(MakeRequest(GET, "/api/controller/schedules/iaq-A-1")).result());
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == route.OnRequest(MakeRequest(POST, "/api/controller/schedules/iaq-A-1", VALID_PROGRAM)).result());

	BOOST_CHECK(dispatcher->deleted.empty());
	BOOST_CHECK(dispatcher->edited.empty());
}

BOOST_AUTO_TEST_CASE(Item_NoDispatcher_Returns503)
{
	HTTP::WebRoute_ControllerSchedule route{ store };   // default: no dispatcher

	const auto del = route.OnRequest(MakeRequest(DELETE_, "/api/controller/schedules/iaq-A-1"));
	BOOST_CHECK(boost::beast::http::status::service_unavailable == del.result());
	BOOST_CHECK_EQUAL(del.body(), "Command dispatcher not available");

	const auto put = route.OnRequest(MakeRequest(PUT, "/api/controller/schedules/iaq-A-1", VALID_PROGRAM));
	BOOST_CHECK(boost::beast::http::status::service_unavailable == put.result());
}

BOOST_AUTO_TEST_CASE(Item_Delete_DispatcherBusy_Returns409Rejected)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::Busy;

	HTTP::WebRoute_ControllerSchedule route{ store, dispatcher };
	const auto resp = route.OnRequest(MakeRequest(DELETE_, "/api/controller/schedules/iaq-A-1"));

	BOOST_CHECK(boost::beast::http::status::conflict == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("status", ""), "rejected");
	BOOST_REQUIRE_EQUAL(dispatcher->deleted.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->deleted[0].id, "iaq-A-1");
}

BOOST_AUTO_TEST_CASE(Item_Put_NonObjectBody_Returns400_NoDispatch)
{
	HTTP::WebRoute_ControllerSchedule route{ store, dispatcher };

	const auto resp = route.OnRequest(MakeRequest(PUT, "/api/controller/schedules/iaq-A-1", "[1,2,3]"));
	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
	BOOST_CHECK_EQUAL(resp.body(), "request body must be a JSON object");

	BOOST_CHECK(boost::beast::http::status::bad_request == route.OnRequest(MakeRequest(PUT, "/api/controller/schedules/iaq-A-1", "{oops")).result());

	BOOST_CHECK(dispatcher->edited.empty());
}

BOOST_AUTO_TEST_CASE(Item_Put_DispatcherNoWriter_Returns503Rejected)
{
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;

	HTTP::WebRoute_ControllerSchedule route{ store, dispatcher };
	const auto resp = route.OnRequest(MakeRequest(PUT, "/api/controller/schedules/iaq-A-1", R"({"target":"Pool Heat","days_of_week":127,"on_local":"11:00","off_local":"15:00"})"));

	BOOST_CHECK(boost::beast::http::status::service_unavailable == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK_EQUAL(body.value("status", ""), "rejected");
	BOOST_CHECK_EQUAL(body["schedule"].value("off_local", ""), "15:00");
	BOOST_REQUIRE_EQUAL(dispatcher->edited.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->edited[0].first.id, "iaq-A-1");
}

BOOST_AUTO_TEST_SUITE_END()
