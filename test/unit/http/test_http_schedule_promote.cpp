#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
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
#include "http/webroute_schedule_promote.h"
#include "interfaces/icommanddispatcher.h"
#include "options/options_scheduling_options.h"
#include "scheduling/controller_schedule.h"
#include "scheduling/schedule.h"
#include "scheduling/scheduler_service.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	class StubDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		std::vector<Scheduling::ControllerSchedule> created;

		CommandResult ToggleByUuid(const boost::uuids::uuid&) override { return CommandResult::Success; }
		CommandResult ToggleByLabel(const std::string&) override { return CommandResult::Success; }
		CommandResult CommandByUuid(const boost::uuids::uuid&, DeviceAction) override { return CommandResult::Success; }
		CommandResult CommandByLabel(const std::string&, DeviceAction) override { return CommandResult::Success; }
		CommandResult SetPoolSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetSpaSetpoint(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorPercentage(std::uint8_t) override { return CommandResult::Success; }
		CommandResult SetChlorinatorBoost(bool) override { return CommandResult::Success; }
		CommandResult SetCirculationMode(Kernel::CirculationModes) override { return CommandResult::Success; }
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds, bool) override { return CommandResult::Success; }
		CommandResult SelectIAQPageButton(std::uint8_t) override { return CommandResult::Success; }
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule& p) override { created.push_back(p); return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	Scheduling::Schedule Point(Scheduling::ActionType type, const std::string& target, std::uint8_t days, int hour, int minute)
	{
		Scheduling::Schedule s;
		s.enabled = true;
		s.days_of_week = days;
		s.hour = hour;
		s.minute = minute;
		s.action.type = type;
		s.action.target = target;
		return s;
	}

	struct Fixture : Test::HubLocatorInjector
	{
		Options::Scheduling::SchedulingSettings settings;
		boost::asio::io_context io;
		std::shared_ptr<Scheduling::SchedulerService> service;
		std::shared_ptr<StubDispatcher> dispatcher{ std::make_shared<StubDispatcher>() };

		Fixture()
		{
			settings.schedules_file = (std::filesystem::temp_directory_path() / "aqualink_promote_route_test.json").string();
			std::error_code ec;
			std::filesystem::remove(settings.schedules_file, ec);
			service = std::make_shared<Scheduling::SchedulerService>(io, *this, settings);
		}
		~Fixture()
		{
			std::error_code ec;
			std::filesystem::remove(settings.schedules_file, ec);
			std::filesystem::remove(settings.schedules_file + ".tmp", ec);
		}

		std::string Create(const Scheduling::Schedule& s) { return service->Create(s).uuid; }

		HTTP::Response Promote(const std::string& uuid)
		{
			HTTP::WebRoute_SchedulePromote route(service, dispatcher);

			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::post);
			req.target("/api/schedules/" + uuid + "/promote");
			req.set(boost::beast::http::field::host, "localhost.localdomain");
			req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
			req.prepare_payload();

			HTTP::Message msg = route.OnRequest(req);

			boost::asio::io_context ioc;
			auto exec = ioc.get_executor();
			Test::MockBeastBasicStreamWithTimeout client_stream(exec);
			Test::MockBeastBasicStreamWithTimeout server_stream(exec);
			server_stream.connect(client_stream);

			boost::beast::error_code ec;
			boost::beast::write(server_stream, std::move(msg), ec);
			BOOST_REQUIRE_MESSAGE(!ec, "write: " << ec.message());
			server_stream.close();
			ioc.poll();

			HTTP::Response resp;
			boost::beast::flat_buffer buf;
			boost::beast::http::read(client_stream, buf, resp, ec);
			BOOST_REQUIRE_MESSAGE(!ec || ec == boost::beast::http::error::end_of_stream, "read: " << ec.message());
			return resp;
		}
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_SchedulePromote, Fixture)

BOOST_AUTO_TEST_CASE(Promote_ValidOnOffPair_QueuesControllerCreate)
{
	const auto on_uuid = Create(Point(Scheduling::ActionType::ButtonOn, "Filter Pump", 0x7f, 9, 0));
	Create(Point(Scheduling::ActionType::ButtonOff, "Filter Pump", 0x7f, 17, 0));

	auto resp = Promote(on_uuid);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->created.size(), 1u);
	BOOST_CHECK_EQUAL(dispatcher->created[0].target, "Filter Pump");
	BOOST_CHECK_EQUAL(dispatcher->created[0].on_hour, 9);
	BOOST_CHECK_EQUAL(dispatcher->created[0].off_hour, 17);
	BOOST_CHECK_EQUAL(dispatcher->created[0].days_of_week, 0x7f);
}

BOOST_AUTO_TEST_CASE(Promote_UnknownUuid_Returns404)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, Promote("does-not-exist").result());
	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_CASE(Promote_NoComplement_Returns422)
{
	// An ON with no matching OFF cannot form a span.
	const auto on_uuid = Create(Point(Scheduling::ActionType::ButtonOn, "Filter Pump", 0x7f, 9, 0));
	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, Promote(on_uuid).result());
	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_CASE(Promote_NonButtonAction_Returns422)
{
	const auto uuid = Create(Point(Scheduling::ActionType::PoolSetpoint, "", 0x7f, 9, 0));
	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, Promote(uuid).result());
	BOOST_CHECK(dispatcher->created.empty());
}

BOOST_AUTO_TEST_SUITE_END()
