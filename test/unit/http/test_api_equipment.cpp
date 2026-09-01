#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

#include <boost/beast/core/buffers_to_string.hpp>
#include <nlohmann/json.hpp>

#include "http/webroute_equipment.h"
#include "http/server/routing/routing.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "types/units_dimensionless.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_httprequestresponse.h"
#include "support/unit_test_onetouchdevice.h"

using namespace AqualinkAutomate;

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_ApiEquipment, Test::OneTouchDevice)

BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_UninitialisedDataHub)
{
	HTTP::Routing::Clear();

	auto route_api_equiment = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	BOOST_REQUIRE(nullptr != route_api_equiment);
	auto route_api_equipment_route = route_api_equiment->Route();
	BOOST_REQUIRE("/api/equipment" == route_api_equipment_route);

	HTTP::Routing::Add(std::move(route_api_equiment));

	{
		auto resp = Test::PerformHttpRequestResponse(route_api_equipment_route);
		BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

		const auto& res_body = resp.body();
		BOOST_TEST_REQUIRE(0 != res_body.length());

		{
			nlohmann::json json_response = nlohmann::json::parse(res_body);

			// The dead GenerateJson_Equipment_Buttons stub (and its empty "buttons"
			// field) was removed; the /api/equipment response no longer carries it.
			BOOST_CHECK(!json_response.contains("buttons"));
			BOOST_REQUIRE(json_response.contains("devices"));
			BOOST_REQUIRE(json_response.contains("stats"));
			BOOST_REQUIRE(json_response.contains("version"));

			BOOST_REQUIRE(json_response["devices"].contains("auxillaries"));
			BOOST_REQUIRE(json_response["devices"].contains("heaters"));
			BOOST_REQUIRE(json_response["devices"].contains("pumps"));

			BOOST_CHECK(json_response["devices"]["auxillaries"].is_null());
			BOOST_CHECK(json_response["devices"]["heaters"].is_null());
			BOOST_CHECK(json_response["devices"]["pumps"].is_null());

			BOOST_REQUIRE(json_response["version"].contains("fw_revision"));
			BOOST_REQUIRE(json_response["version"].contains("model_number"));

			BOOST_CHECK_EQUAL("", json_response["version"]["fw_revision"]);
			BOOST_CHECK_EQUAL("", json_response["version"]["model_number"]);
		}
	}
}

BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_InitialisedDataHub)
{
	HTTP::Routing::Clear();

	InitialiseOneTouchDevice();
	EquipmentOnOff_Page1();
	EquipmentOnOff_Page2();
	EquipmentOnOff_Page3();

	auto check_for_device = [](const auto& label, const auto& state, const auto& device_vec) -> bool
	{
		bool was_found = false;

		for (const auto& device : device_vec)
		{
			// Skip devices that don't have required fields or don't match
			if (!device.contains("label") || !device.contains("state"))
			{
				continue;
			}
			if (label != device.at("label") || state != device.at("state"))
			{
				continue;
			}
			was_found = true;
			break;
		}

		return was_found;
	};

	boost::asio::io_context io_context;

	auto route_api_equiment = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	BOOST_REQUIRE(nullptr != route_api_equiment);
	auto route_api_equipment_route = route_api_equiment->Route();
	BOOST_REQUIRE("/api/equipment" == route_api_equipment_route);

	HTTP::Routing::Add(std::move(route_api_equiment));

	{
		auto resp = Test::PerformHttpRequestResponse(route_api_equipment_route);
		BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

		const auto& res_body = resp.body();
		BOOST_TEST_REQUIRE(0 != res_body.length());

		{
			nlohmann::json json_response = nlohmann::json::parse(res_body);

			// The dead GenerateJson_Equipment_Buttons stub (and its empty "buttons"
			// field) was removed; the /api/equipment response no longer carries it.
			BOOST_CHECK(!json_response.contains("buttons"));
			BOOST_REQUIRE(json_response.contains("devices"));
			BOOST_REQUIRE(json_response.contains("stats"));
			BOOST_REQUIRE(json_response.contains("version"));

			BOOST_REQUIRE(json_response["devices"].contains("auxillaries"));
			BOOST_REQUIRE(json_response["devices"].contains("heaters"));
			BOOST_REQUIRE(json_response["devices"].contains("pumps"));

			BOOST_CHECK_EQUAL(15, json_response["devices"]["auxillaries"].size());
			BOOST_CHECK_EQUAL(2, json_response["devices"]["heaters"].size());
			BOOST_CHECK_EQUAL(2, json_response["devices"]["pumps"].size());

			BOOST_CHECK(check_for_device("Aux1", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux2", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux3", "On", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux4", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux5", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux6", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(!check_for_device("Aux7", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(!check_for_device("Aux8", "N/A", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B1", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B2", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B3", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B4", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B5", "On", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B6", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B7", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Aux B8", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Extra Aux", "Off", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(check_for_device("Pool Heat", "Enabled", json_response["devices"]["heaters"]));
			BOOST_CHECK(check_for_device("Spa Heat", "Off", json_response["devices"]["heaters"]));
			BOOST_CHECK(!check_for_device("Solar Heat", "N/A", json_response["devices"]["heaters"]));
			BOOST_CHECK(!check_for_device("Heat Pump", "N/A", json_response["devices"]["heaters"]));
			BOOST_CHECK(check_for_device("Pool Pump", "Unknown", json_response["devices"]["pumps"]));
			BOOST_CHECK(check_for_device("Spa Pump", "Running", json_response["devices"]["pumps"]));
			BOOST_CHECK(!check_for_device("Filter Pump", "N/A", json_response["devices"]["pumps"]));

			// Check for mis-parsing of the all off command.
			BOOST_CHECK(!check_for_device("All Off", "Unknown", json_response["devices"]["auxillaries"]));
			BOOST_CHECK(!check_for_device("All Off", "Unknown", json_response["devices"]["heaters"]));
			BOOST_CHECK(!check_for_device("All Off", "Unknown", json_response["devices"]["pumps"]));

			BOOST_REQUIRE(json_response["version"].contains("fw_revision"));
			BOOST_REQUIRE(json_response["version"].contains("model_number"));

			BOOST_CHECK_EQUAL("REV T.0.1", json_response["version"]["fw_revision"]);
			BOOST_CHECK_EQUAL("B0029221", json_response["version"]["model_number"]);
		}
	}
}

BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_ChemistryNestedShape_NoChlorinator)
{
	HTTP::Routing::Clear();

	// No chlorinator discovered, no ORP/pH sensor: chemistry block is nested,
	// salt defaults to 0, ORP/pH are null placeholders and chlorinator is null.
	auto route = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	BOOST_REQUIRE(nullptr != route);
	const auto route_url = route->Route();
	HTTP::Routing::Add(std::move(route));

	auto resp = Test::PerformHttpRequestResponse(route_url);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto json_response = nlohmann::json::parse(resp.body());

	BOOST_REQUIRE(json_response.contains("chemistry"));
	const auto& chemistry = json_response["chemistry"];

	BOOST_REQUIRE(chemistry.contains("salt_ppm"));
	BOOST_CHECK(chemistry["salt_ppm"].is_number());
	BOOST_CHECK_EQUAL(0, chemistry["salt_ppm"]);

	// ORP / pH are placeholders today — 0 on the wire is surfaced as null so the
	// UI renders a "--" placeholder rather than a misleading zero.
	BOOST_REQUIRE(chemistry.contains("orp_mv"));
	BOOST_CHECK(chemistry["orp_mv"].is_null());
	BOOST_REQUIRE(chemistry.contains("ph"));
	BOOST_CHECK(chemistry["ph"].is_null());

	// No chlorinator on the bus -> the chlorinator sub-object is null.
	BOOST_REQUIRE(chemistry.contains("chlorinator"));
	BOOST_CHECK(chemistry["chlorinator"].is_null());
}

BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_ChemistryNestedShape_WithChlorinator)
{
	using namespace Kernel::AuxillaryTraitsTypes;

	HTTP::Routing::Clear();

	// Seed a chlorinator AuxillaryDevice with the SWG traits the AquariteDevice
	// would push, plus a salt level, then assert the nested chemistry payload
	// surfaces them under chemistry.chlorinator + chemistry.salt_ppm.
	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlorinator->AuxillaryTraits.Set(GeneratingPercentageTrait{}, static_cast<uint8_t>(60));
		chlorinator->AuxillaryTraits.Set(DutyCycleTrait{}, static_cast<uint8_t>(60));
		chlorinator->AuxillaryTraits.Set(ChlorinatorPoolSetpointTrait{}, static_cast<uint8_t>(45));
		chlorinator->AuxillaryTraits.Set(ChlorinatorSpaSetpointTrait{}, static_cast<uint8_t>(50));
		chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		chlorinator->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Ok);
		chlorinator->AuxillaryTraits.Set(ChlorinatorHealthFlagsTrait{}, std::set<Kernel::ChlorinatorHealth>{ Kernel::ChlorinatorHealth::Ok });
		DataHub().Devices.Add(std::move(chlorinator));
	}

	DataHub().SaltLevel(3200 * Units::ppm);

	auto route = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	BOOST_REQUIRE(nullptr != route);
	const auto route_url = route->Route();
	HTTP::Routing::Add(std::move(route));

	auto resp = Test::PerformHttpRequestResponse(route_url);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto json_response = nlohmann::json::parse(resp.body());

	BOOST_REQUIRE(json_response.contains("chemistry"));
	const auto& chemistry = json_response["chemistry"];

	BOOST_REQUIRE(chemistry.contains("salt_ppm"));
	BOOST_CHECK_EQUAL(3200, chemistry["salt_ppm"]);

	// ORP / pH still null (no sensor seeded).
	BOOST_CHECK(chemistry["orp_mv"].is_null());
	BOOST_CHECK(chemistry["ph"].is_null());

	// Nested chlorinator block populated from the DataHub traits.
	BOOST_REQUIRE(chemistry.contains("chlorinator"));
	const auto& chlorinator = chemistry["chlorinator"];
	BOOST_REQUIRE(!chlorinator.is_null());

	BOOST_REQUIRE(chlorinator.contains("generating_percent"));
	BOOST_CHECK_EQUAL(60, chlorinator["generating_percent"]);
	BOOST_REQUIRE(chlorinator.contains("duty_cycle"));
	BOOST_CHECK_EQUAL(60, chlorinator["duty_cycle"]);
	BOOST_REQUIRE(chlorinator.contains("status"));
	BOOST_CHECK_EQUAL("On", chlorinator["status"]);
	BOOST_REQUIRE(chlorinator.contains("health"));
	BOOST_CHECK_EQUAL("Ok", chlorinator["health"]);
	BOOST_REQUIRE(chlorinator.contains("health_flags"));
	BOOST_REQUIRE(chlorinator["health_flags"].is_array());
	BOOST_REQUIRE_EQUAL(1u, chlorinator["health_flags"].size());
	BOOST_CHECK_EQUAL("Ok", chlorinator["health_flags"][0]);

	// Configured Pool / Spa setpoints surfaced, plus the resolved headline target. With no
	// active spa body configured, the headline setpoint resolves to the Pool value.
	BOOST_REQUIRE(chlorinator.contains("pool_setpoint_percent"));
	BOOST_CHECK_EQUAL(45, chlorinator["pool_setpoint_percent"]);
	BOOST_REQUIRE(chlorinator.contains("spa_setpoint_percent"));
	BOOST_CHECK_EQUAL(50, chlorinator["spa_setpoint_percent"]);
	BOOST_REQUIRE(chlorinator.contains("setpoint_percent"));
	BOOST_CHECK_EQUAL(45, chlorinator["setpoint_percent"]);

	// The cell is producing, so the reason says so rather than leaving the reader to infer
	// it from the percentage.
	BOOST_REQUIRE(chlorinator.contains("generating_reason"));
	BOOST_CHECK_EQUAL("Generating", chlorinator["generating_reason"]);
}

//-----------------------------------------------------------------------------
// REGRESSION: a chlorinator reporting 0% output must say WHY.
//
// The instantaneous output is 0 whenever the cell is idle, so on its own it reads
// as "the chlorinator is off or broken" even when it is configured, healthy, and
// simply waiting for the filter pump -- which is the normal state for most of the
// day. generating_reason is what separates those cases, and it is derived
// server-side so the web UI, MQTT and Home Assistant cannot disagree.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_ChlorinatorIdle_ReportsPumpOffReason)
{
	using namespace Kernel::AuxillaryTraitsTypes;

	HTTP::Routing::Clear();

	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlorinator->AuxillaryTraits.Set(GeneratingPercentageTrait{}, static_cast<uint8_t>(0));
		chlorinator->AuxillaryTraits.Set(ChlorinatorPoolSetpointTrait{}, static_cast<uint8_t>(45));
		chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		chlorinator->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Ok);
		DataHub().Devices.Add(std::move(chlorinator));

		// A filter pump that is NOT running -- the reason the cell is idle.
		auto pump = std::make_shared<Kernel::AuxillaryDevice>();
		pump->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Pump);
		pump->AuxillaryTraits.Set(PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);
		pump->AuxillaryTraits.Set(LabelTrait{}, std::string{ "Filter Pump" });
		pump->AuxillaryTraits.Set(PumpStatusTrait{}, Kernel::PumpStatuses::Off);
		DataHub().Devices.Add(std::move(pump));
	}

	auto route = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	const auto route_url = route->Route();
	HTTP::Routing::Add(std::move(route));

	const auto json_response = nlohmann::json::parse(Test::PerformHttpRequestResponse(route_url).body());
	const auto& chlorinator = json_response["chemistry"]["chlorinator"];
	BOOST_REQUIRE(!chlorinator.is_null());

	// Output is 0 (correct) -- and the payload now explains it rather than leaving "0%" to
	// be read as a fault. The configured target is still reported, so the UI can show what
	// the cell WILL produce once there is flow.
	BOOST_CHECK_EQUAL(0, chlorinator["generating_percent"]);
	BOOST_REQUIRE(chlorinator.contains("generating_reason"));
	BOOST_CHECK_EQUAL("PumpOff", chlorinator["generating_reason"]);
	BOOST_CHECK_EQUAL(45, chlorinator["setpoint_percent"]);
}

// When no menu scrape has populated a Pool/Spa setpoint, the headline setpoint_percent falls
// back to the passive last-known generating %, while the per-body setpoints remain null.
BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_ChlorinatorSetpointFallback)
{
	using namespace Kernel::AuxillaryTraitsTypes;

	HTTP::Routing::Clear();

	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		// No pool/spa setpoint scraped yet; only the passive last-known generating %.
		chlorinator->AuxillaryTraits.Set(ChlorinatorLastGeneratingTrait{}, static_cast<uint8_t>(55));
		DataHub().Devices.Add(std::move(chlorinator));
	}

	auto route = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	const auto route_url = route->Route();
	HTTP::Routing::Add(std::move(route));

	auto resp = Test::PerformHttpRequestResponse(route_url);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto json_response = nlohmann::json::parse(resp.body());
	const auto& chlorinator = json_response["chemistry"]["chlorinator"];
	BOOST_REQUIRE(!chlorinator.is_null());

	BOOST_CHECK(chlorinator["pool_setpoint_percent"].is_null());
	BOOST_CHECK(chlorinator["spa_setpoint_percent"].is_null());
	BOOST_REQUIRE(chlorinator.contains("setpoint_percent"));
	BOOST_CHECK_EQUAL(55, chlorinator["setpoint_percent"]);

	// Neither ChlorinatorHealthTrait nor ChlorinatorHealthFlagsTrait was seeded here ->
	// both fall back to Unknown, and health_flags mirrors that as a single-element array
	// rather than an empty one or a null.
	BOOST_REQUIRE(chlorinator.contains("health"));
	BOOST_CHECK_EQUAL("Unknown", chlorinator["health"]);
	BOOST_REQUIRE(chlorinator.contains("health_flags"));
	BOOST_REQUIRE(chlorinator["health_flags"].is_array());
	BOOST_REQUIRE_EQUAL(1u, chlorinator["health_flags"].size());
	BOOST_CHECK_EQUAL("Unknown", chlorinator["health_flags"][0]);
}

// The wire status byte is a true bitfield: more than one health flag can be active at
// once (e.g. a low-salt warning together with a check-PCB fault). health_flags must
// carry every one, while health stays the single worst-of-the-set value.
BOOST_AUTO_TEST_CASE(Test_HttpRoutes_ApiEquipment_ChlorinatorHealthFlags_MultipleActive)
{
	using namespace Kernel::AuxillaryTraitsTypes;

	HTTP::Routing::Clear();

	{
		auto chlorinator = std::make_shared<Kernel::AuxillaryDevice>();
		chlorinator->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlorinator->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		chlorinator->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Error_CheckPCB);
		chlorinator->AuxillaryTraits.Set(ChlorinatorHealthFlagsTrait{}, std::set<Kernel::ChlorinatorHealth>{
			Kernel::ChlorinatorHealth::Warning_LowSalt, Kernel::ChlorinatorHealth::Error_CheckPCB });
		DataHub().Devices.Add(std::move(chlorinator));
	}

	auto route = std::make_unique<HTTP::WebRoute_Equipment>(*this);
	const auto route_url = route->Route();
	HTTP::Routing::Add(std::move(route));

	auto resp = Test::PerformHttpRequestResponse(route_url);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto json_response = nlohmann::json::parse(resp.body());
	const auto& chlorinator = json_response["chemistry"]["chlorinator"];
	BOOST_REQUIRE(!chlorinator.is_null());

	BOOST_REQUIRE(chlorinator.contains("health"));
	BOOST_CHECK_EQUAL("Error_CheckPCB", chlorinator["health"]);

	BOOST_REQUIRE(chlorinator.contains("health_flags"));
	BOOST_REQUIRE(chlorinator["health_flags"].is_array());
	BOOST_REQUIRE_EQUAL(2u, chlorinator["health_flags"].size());
	// std::set<ChlorinatorHealth> iterates in enum-declaration order (Warning_LowSalt is
	// declared before Error_CheckPCB), which is what json_data_hub/webroute both mirror.
	BOOST_CHECK_EQUAL("Warning_LowSalt", chlorinator["health_flags"][0]);
	BOOST_CHECK_EQUAL("Error_CheckPCB", chlorinator["health_flags"][1]);
}

BOOST_AUTO_TEST_SUITE_END()
