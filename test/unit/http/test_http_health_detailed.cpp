#include <string>

#include <boost/test/unit_test.hpp>

#include <boost/beast/http/verb.hpp>

#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_health_detailed.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/pool_configurations.h"
#include "kernel/system_boards.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{

	HTTP::Request MakeRequest(boost::beast::http::verb method = boost::beast::http::verb::get)
	{
		HTTP::Request req;
		req.version(11);
		req.method(method);
		req.target(HTTP::HEALTH_DETAILED_ROUTE_URL);
		req.set(boost::beast::http::field::host, "localhost.localdomain");
		return req;
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_HttpHealthDetailed)

// The detailed report exposes internal subsystem state, so it must NOT opt out of
// the security policy (the bare /api/health liveness probe is the exempt one).
BOOST_AUTO_TEST_CASE(Test_HealthDetailed_RequiresAuthentication)
{
	Test::HubLocatorInjector hub_locator;
	HTTP::WebRoute_HealthDetailed route(hub_locator);

	BOOST_CHECK(route.RequiresAuthentication());
}

// Fresh system: pool configuration is Unknown -> not ready -> 503 with status
// "starting", so the endpoint can act as a readiness probe.
BOOST_AUTO_TEST_CASE(Test_HealthDetailed_NotReady_Returns503)
{
	Test::HubLocatorInjector hub_locator;
	HTTP::WebRoute_HealthDetailed route(hub_locator);

	auto req = MakeRequest();
	auto resp = route.OnRequest(req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body.at("status").get<std::string>(), "starting");
	BOOST_CHECK_EQUAL(body.at("ready").get<bool>(), false);
	BOOST_CHECK_EQUAL(body.at("checks").at("configuration").at("ready").get<bool>(), false);
	// MQTT is not registered by the injector -> reported present-but-disabled.
	BOOST_CHECK_EQUAL(body.at("checks").at("mqtt").at("enabled").get<bool>(), false);
}

// Once the pool configuration is determined the system is ready -> 200, status "ok".
BOOST_AUTO_TEST_CASE(Test_HealthDetailed_Ready_Returns200)
{
	Test::HubLocatorInjector hub_locator;

	auto data_hub = hub_locator.Find<Kernel::DataHub>();
	BOOST_REQUIRE(nullptr != data_hub);
	data_hub->PoolConfiguration = Kernel::PoolConfigurations::SingleBody;

	HTTP::WebRoute_HealthDetailed route(hub_locator);

	auto req = MakeRequest();
	auto resp = route.OnRequest(req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body.at("status").get<std::string>(), "ok");
	BOOST_CHECK_EQUAL(body.at("ready").get<bool>(), true);
	// The diagnostics page renders these verbatim, so the endpoint must emit
	// human-readable labels rather than the raw enum identifier ("SingleBody").
	BOOST_CHECK_EQUAL(body.at("checks").at("configuration").at("pool_configuration").get<std::string>(), "Single Body");
	BOOST_CHECK(body.at("checks").at("equipment").contains("device_count"));
	BOOST_CHECK(body.contains("uptime_seconds"));
	BOOST_CHECK(body.at("version").contains("name"));
}

// Regression: the diagnostics "system board" and "pool config" fields must be
// decoded into human-readable labels, not leaked as raw C++ enum identifiers
// (e.g. "RS8_Combo" / "DualBody_SharedEquipment").
BOOST_AUTO_TEST_CASE(Test_HealthDetailed_BoardAndConfig_AreDisplayLabels)
{
	Test::HubLocatorInjector hub_locator;

	auto data_hub = hub_locator.Find<Kernel::DataHub>();
	BOOST_REQUIRE(nullptr != data_hub);
	data_hub->PoolConfiguration = Kernel::PoolConfigurations::DualBody_SharedEquipment;
	data_hub->SystemBoard = Kernel::SystemBoards::RS8_Combo;

	HTTP::WebRoute_HealthDetailed route(hub_locator);

	auto resp = route.OnRequest(MakeRequest());
	const auto body = nlohmann::json::parse(resp.body());
	const auto& config = body.at("checks").at("configuration");

	BOOST_CHECK_EQUAL(config.at("system_board").get<std::string>(), "RS-8 Combo");
	BOOST_CHECK_EQUAL(config.at("pool_configuration").get<std::string>(), "Dual Body (Shared Equipment)");
}

// Non-GET methods are rejected with 405.
BOOST_AUTO_TEST_CASE(Test_HealthDetailed_NonGet_Returns405)
{
	Test::HubLocatorInjector hub_locator;
	HTTP::WebRoute_HealthDetailed route(hub_locator);

	auto req = MakeRequest(boost::beast::http::verb::post);
	auto resp = route.OnRequest(req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, resp.result());
}

BOOST_AUTO_TEST_SUITE_END()
