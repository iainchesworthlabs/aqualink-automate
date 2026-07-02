#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "http/websocket_event.h"
#include "http/websocket_equipment.h"
#include "http/server/routing/routing.h"
#include "kernel/data_hub.h"
#include "kernel/hub_events/data_hub_config_event.h"
#include "kernel/hub_events/data_hub_config_event_chemistry.h"
#include "kernel/hub_events/data_hub_config_event_circulation.h"
#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "kernel/orp.h"

#include "support/unit_test_httprequestresponse.h"
#include "support/unit_test_onetouchdevice.h"
#include "support/unit_test_ostream_support.h"
#include "support/unit_test_wsrequestresponse.h"

using namespace AqualinkAutomate;

BOOST_FIXTURE_TEST_SUITE(TestSuite_WebsocketRoutes_WsEquipment, Test::OneTouchDevice)

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_WebSocket_ChemistryEventConversion)
{
	{
		auto config_event_null = std::make_shared<Kernel::DataHub_ConfigEvent_Chemistry>();
		auto config_event_base = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent>(config_event_null);
		BOOST_REQUIRE(nullptr != config_event_base);
		HTTP::WebSocket_Event wse1(config_event_base);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::ChemistryUpdate, wse1.Type());
		auto wse1_string = wse1.Payload();
		auto wse1_json = nlohmann::json::parse(wse1_string);
		auto wse1_json_type = wse1_json["type"];
		auto wse1_json_payload = wse1_json["payload"];
		BOOST_CHECK_EQUAL("ChemistryUpdate", wse1_json_type);
		BOOST_CHECK(wse1_json_payload.is_null());
	}

	{
		auto config_event_chem = std::make_shared<Kernel::DataHub_ConfigEvent_Chemistry>();
		BOOST_REQUIRE(nullptr != config_event_chem);
		config_event_chem->ORP(650);
		config_event_chem->pH(7.5f);
		HTTP::WebSocket_Event wse2(config_event_chem);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::ChemistryUpdate, wse2.Type());
		auto wse2_string = wse2.Payload();
		auto wse2_json = nlohmann::json::parse(wse2_string);
		auto wse2_json_type = wse2_json["type"];
		auto wse2_json_payload = wse2_json["payload"];
		BOOST_CHECK_EQUAL("ChemistryUpdate", wse2_json_type);
		BOOST_CHECK(wse2_json["payload"].contains("orp"));
		BOOST_CHECK(wse2_json["payload"]["orp"].is_number());
		BOOST_CHECK_EQUAL(650, wse2_json["payload"]["orp"]);
		BOOST_CHECK(wse2_json["payload"].contains("ph"));
		BOOST_CHECK(wse2_json["payload"]["ph"].is_number_float());
		BOOST_CHECK_EQUAL(7.5f, wse2_json["payload"]["ph"]);
	}

	{
		// Regression: pH is stored as a float32 rounded to 0.1. 7.1 is NOT exactly representable,
		// so promoting it raw to double leaks 7.099999904632568 into the serialized payload. The
		// ToJSON path must re-round at the JSON boundary so the dumped value reads cleanly as 7.1.
		auto config_event_chem = std::make_shared<Kernel::DataHub_ConfigEvent_Chemistry>();
		BOOST_REQUIRE(nullptr != config_event_chem);
		config_event_chem->pH(7.1f);
		HTTP::WebSocket_Event wse3(config_event_chem);

		auto wse3_json = nlohmann::json::parse(wse3.Payload());
		BOOST_REQUIRE(wse3_json["payload"].contains("ph"));
		// Assert on the serialized text: the bug surfaces only in the dumped string form.
		BOOST_CHECK_EQUAL(wse3_json["payload"]["ph"].dump(), "7.1");
		BOOST_CHECK_EQUAL(7.1, wse3_json["payload"]["ph"].get<double>());
	}
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_WebSocket_TemperatureEventConversion)
{
	{
		auto config_event_null = std::make_shared<Kernel::DataHub_ConfigEvent_Temperature>();
		auto config_event_base = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent>(config_event_null);
		BOOST_REQUIRE(nullptr != config_event_base);
		HTTP::WebSocket_Event wse1(config_event_base);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::TemperatureUpdate, wse1.Type());
		auto wse1_string = wse1.Payload();
		auto wse1_json = nlohmann::json::parse(wse1_string);
		auto wse1_json_type = wse1_json["type"];
		auto wse1_json_payload = wse1_json["payload"];
		BOOST_CHECK_EQUAL("TemperatureUpdate", wse1_json_type);
		BOOST_CHECK(wse1_json_payload.is_null());
	}

	// Temperatures ride as raw dual-unit objects ({celsius, fahrenheit}),
	// matching the REST /api/equipment shape. Regression: the old path emitted
	// pre-formatted strings with the unit HARDCODED to Celsius, ignoring the
	// Temperature_DisplayUnits preference (docs/i18n.md \u2014 display formatting
	// is the frontend's job).
	{
		auto config_event_temp = std::make_shared<Kernel::DataHub_ConfigEvent_Temperature>();
		BOOST_REQUIRE(nullptr != config_event_temp);
		Utility::TemperatureStringConverter pool_temp("Pool        90`F");
		config_event_temp->PoolTemp(pool_temp().value()); // Make sure to use the right separator character --> `
		HTTP::WebSocket_Event wse2(config_event_temp);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::TemperatureUpdate, wse2.Type());
		auto wse2_string = wse2.Payload();
		auto wse2_json = nlohmann::json::parse(wse2_string);
		auto wse2_json_type = wse2_json["type"];
		auto wse2_json_payload = wse2_json["payload"];
		BOOST_CHECK_EQUAL("TemperatureUpdate", wse2_json_type);
		BOOST_CHECK(wse2_json["payload"].contains("pool_temp"));
		BOOST_CHECK(wse2_json["payload"]["pool_temp"].is_object());
		BOOST_CHECK_EQUAL(90.0, wse2_json["payload"]["pool_temp"]["fahrenheit"].get<double>());
	}

	{
		auto config_event_temp = std::make_shared<Kernel::DataHub_ConfigEvent_Temperature>();
		BOOST_REQUIRE(nullptr != config_event_temp);
		config_event_temp->PoolTemp(Utility::TemperatureStringConverter("Pool        21`C")().value()); // Make sure to use the right separator character --> `
		config_event_temp->SpaTemp(Utility::TemperatureStringConverter("Spa         39`C")().value()); // Make sure to use the right separator character --> `
		config_event_temp->AirTemp(Utility::TemperatureStringConverter("Air         16`C")().value()); // Make sure to use the right separator character --> `
		HTTP::WebSocket_Event wse3(config_event_temp);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::TemperatureUpdate, wse3.Type());
		auto wse3_string = wse3.Payload();
		auto wse3_json = nlohmann::json::parse(wse3_string);
		auto wse3_json_type = wse3_json["type"];
		auto wse3_json_payload = wse3_json["payload"];
		BOOST_CHECK_EQUAL("TemperatureUpdate", wse3_json_type);
		BOOST_CHECK(wse3_json["payload"].contains("pool_temp"));
		BOOST_CHECK(wse3_json["payload"]["pool_temp"].is_object());
		BOOST_CHECK_EQUAL(21.0, wse3_json["payload"]["pool_temp"]["celsius"].get<double>());
		BOOST_CHECK_EQUAL(69.8, wse3_json["payload"]["pool_temp"]["fahrenheit"].get<double>());
		BOOST_CHECK(wse3_json["payload"].contains("spa_temp"));
		BOOST_CHECK(wse3_json["payload"]["spa_temp"].is_object());
		BOOST_CHECK_EQUAL(39.0, wse3_json["payload"]["spa_temp"]["celsius"].get<double>());
		BOOST_CHECK(wse3_json["payload"].contains("air_temp"));
		BOOST_CHECK(wse3_json["payload"]["air_temp"].is_object());
		BOOST_CHECK_EQUAL(16.0, wse3_json["payload"]["air_temp"]["celsius"].get<double>());
	}
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_WebSocket_CirculationEventConversion)
{
	{
		// Empty event still routes as a CirculationUpdate; defaults to Pool mode, no bodies.
		auto config_event_null = std::make_shared<Kernel::DataHub_ConfigEvent_Circulation>();
		auto config_event_base = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent>(config_event_null);
		BOOST_REQUIRE(nullptr != config_event_base);
		HTTP::WebSocket_Event wse1(config_event_base);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::CirculationUpdate, wse1.Type());
		auto wse1_json = nlohmann::json::parse(wse1.Payload());
		BOOST_CHECK_EQUAL("CirculationUpdate", wse1_json["type"]);
		BOOST_CHECK_EQUAL("Pool", wse1_json["payload"]["mode"]);
		BOOST_CHECK_EQUAL(false, wse1_json["payload"]["spa_mode"]);
		BOOST_CHECK(wse1_json["payload"]["active_body"].is_null());
		BOOST_CHECK(wse1_json["payload"]["bodies"].is_array());
		BOOST_CHECK_EQUAL(0U, wse1_json["payload"]["bodies"].size());
	}

	{
		// Spa mode with both bodies: spa active, pool inactive.
		auto config_event_circ = std::make_shared<Kernel::DataHub_ConfigEvent_Circulation>();
		config_event_circ->Mode(Kernel::CirculationModes::Spa);
		config_event_circ->AddBody(Kernel::BodyOfWaterIds::Pool, false);
		config_event_circ->AddBody(Kernel::BodyOfWaterIds::Spa, true);
		HTTP::WebSocket_Event wse2(config_event_circ);

		BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::CirculationUpdate, wse2.Type());
		auto wse2_json = nlohmann::json::parse(wse2.Payload());
		BOOST_CHECK_EQUAL("CirculationUpdate", wse2_json["type"]);
		BOOST_CHECK_EQUAL("Spa", wse2_json["payload"]["mode"]);
		BOOST_CHECK_EQUAL(true, wse2_json["payload"]["spa_mode"]);
		BOOST_CHECK_EQUAL("Spa", wse2_json["payload"]["active_body"]);
		BOOST_REQUIRE_EQUAL(2U, wse2_json["payload"]["bodies"].size());
		BOOST_CHECK_EQUAL("Pool", wse2_json["payload"]["bodies"][0]["id"]);
		BOOST_CHECK_EQUAL(false, wse2_json["payload"]["bodies"][0]["is_active"]);
		BOOST_CHECK_EQUAL("Spa", wse2_json["payload"]["bodies"][1]["id"]);
		BOOST_CHECK_EQUAL(true, wse2_json["payload"]["bodies"][1]["is_active"]);
	}
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_WebSocket_PublishChemistryUpdate)
{
	boost::beast::error_code ec;

	InitialiseOneTouchDevice();
	EquipmentOnOff_Page1();
	EquipmentOnOff_Page2();
	EquipmentOnOff_Page3();

	/* {
		auto route_ws_equipment = std::make_unique<HTTP::WebSocket_Equipment>(*this);
		BOOST_REQUIRE(nullptr != route_ws_equipment);
		HTTP::Routing::Add(std::move(route_ws_equipment));

		auto [websocket_buffer, bytes_read] = Test::PerformHttpWsUpgradeResponse("/ws/equipment");

		std::array<uint8_t, 1024> websocket_buffer;
		BOOST_TEST_REQUIRE(0 == tro->local_stream.buffer().size());

		{
			DataHub().ORP(650);
			io_context.poll(); // ASYNC_READ

			auto size = tro->local_stream.buffer().size();
			const auto bytes_read = tro->local_stream.read_some(boost::asio::buffer(websocket_buffer), ec);
			BOOST_TEST_REQUIRE(0 == ec.value());
			BOOST_TEST_REQUIRE(0 != bytes_read);

			nlohmann::json wse_json;
			BOOST_REQUIRE_NO_THROW(wse_json = nlohmann::json::parse(websocket_buffer.data() + 2, websocket_buffer.data() + bytes_read));

			BOOST_REQUIRE(wse_json.contains("type"));
			BOOST_CHECK_EQUAL("ChemistryUpdate", wse_json["type"]);
			BOOST_REQUIRE(wse_json.contains("payload"));
			BOOST_REQUIRE(wse_json["payload"].contains("orp"));
			BOOST_CHECK_EQUAL(650, wse_json["payload"]["orp"]);
		}

		{
			DataHub().pH(7.5);
			io_context.poll(); // ASYNC_READ

			auto size = tro->local_stream.buffer().size();
			const auto bytes_read = tro->local_stream.read_some(boost::asio::buffer(websocket_buffer), ec);
			BOOST_TEST_REQUIRE(0 == ec.value());
			BOOST_TEST_REQUIRE(0 != bytes_read);

			nlohmann::json wse_json;
			BOOST_REQUIRE_NO_THROW(wse_json = nlohmann::json::parse(websocket_buffer.data() + 2, websocket_buffer.data() + bytes_read));

			BOOST_REQUIRE(wse_json.contains("type"));
			BOOST_CHECK_EQUAL("ChemistryUpdate", wse_json["type"]);
			BOOST_REQUIRE(wse_json.contains("payload"));
			BOOST_REQUIRE(wse_json["payload"].contains("ph"));
			BOOST_CHECK_EQUAL(7.5, wse_json["payload"]["ph"]);
		}

		{
			DataHub().SaltLevel(4000 * ppm);
			io_context.poll(); // ASYNC_READ

			auto size = tro->local_stream.buffer().size();
			const auto bytes_read = tro->local_stream.read_some(boost::asio::buffer(websocket_buffer), ec);
			BOOST_TEST_REQUIRE(0 == ec.value());
			BOOST_TEST_REQUIRE(0 != bytes_read);

			nlohmann::json wse_json;
			BOOST_REQUIRE_NO_THROW(wse_json = nlohmann::json::parse(websocket_buffer.data() + 2, websocket_buffer.data() + bytes_read));

			BOOST_REQUIRE(wse_json.contains("type"));
			BOOST_CHECK_EQUAL("ChemistryUpdate", wse_json["type"]);
			BOOST_REQUIRE(wse_json.contains("payload"));
			BOOST_REQUIRE(wse_json["payload"].contains("salt_level"));
			BOOST_CHECK_EQUAL(4000, wse_json["payload"]["salt_level"]);
		}
	}
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_WebSocket_PublishTemperatureUpdate)
{
	boost::beast::error_code ec;

	InitialiseOneTouchDevice();
	EquipmentOnOff_Page1();
	EquipmentOnOff_Page2();
	EquipmentOnOff_Page3();

	{
		auto route_ws_equipment = std::make_unique<HTTP::WebSocket_Equipment>(*this);
		BOOST_REQUIRE(nullptr != route_ws_equipment);
		HTTP::Routing::Add(std::move(route_ws_equipment));

		auto&& tro = Test::PerformHttpWsUpgradeResponse("/ws/equipment");

		std::array<uint8_t, 1024> websocket_buffer;
		BOOST_TEST_REQUIRE(0 == tro->local_stream.buffer().size());

		{
			DataHub().PoolTemp(Utility::TemperatureStringConverter("Pool        38`C")().value());
			io_context.poll(); // ASYNC_READ

			auto size = tro->local_stream.buffer().size();
			const auto bytes_read = tro->local_stream.read_some(boost::asio::buffer(websocket_buffer), ec);
			BOOST_TEST_REQUIRE(0 == ec.value());
			BOOST_TEST_REQUIRE(0 != bytes_read);

			nlohmann::json wse_json;
			BOOST_REQUIRE_NO_THROW(wse_json = nlohmann::json::parse(websocket_buffer.data() + 2, websocket_buffer.data() + bytes_read));

			BOOST_REQUIRE(wse_json.contains("type"));
			BOOST_CHECK_EQUAL("TemperatureUpdate", wse_json["type"]);
			BOOST_REQUIRE(wse_json.contains("payload"));
			BOOST_REQUIRE(wse_json["payload"].contains("pool_temp"));
			BOOST_CHECK_EQUAL("38\u00B0C", wse_json["payload"]["pool_temp"]);
		}
	}*/
}

//
// Regression tests for WU-HTTP-WS:
//   - scoped_connection prevents a use-after-free when the handler is destroyed
//     while a hub remains alive and subsequently fires a signal.
//   - the empty-connection short-circuit skips broadcasting when nobody is listening.
//   - a single broadcast reaches every connected client (shared-payload broadcast).
//

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_ScopedConnectionDisconnectsOnDestroy)
{
	// Build a handler, then destroy it while the (fixture-owned) DataHub stays alive.
	// Firing ConfigUpdateSignal afterwards must NOT touch the freed handler (no UAF).
	{
		auto handler = std::make_unique<HTTP::WebSocket_Equipment>(*this);
		BOOST_REQUIRE(nullptr != handler);
		// Handler destroyed at end of scope; scoped_connection disconnects the slot.
	}

	// If the slot were a plain boost::signals2::connection, this would invoke a
	// lambda capturing a dangling `this`. With scoped_connection it is a no-op.
	BOOST_CHECK_NO_THROW(DataHub().ORP(Kernel::ORP(650)));
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_EmptyConnectionsSkipsBroadcast)
{
	HTTP::WebSocket_Equipment handler(*this);

	// No connections open: a config update must not enqueue anything for connection 1.
	DataHub().ORP(Kernel::ORP(650));

	BOOST_CHECK(!handler.DequeueMessage(1).has_value());
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEquipment_BroadcastReachesAllConnections)
{
	HTTP::WebSocket_Equipment handler(*this);

	const auto conn_a = handler.OnOpen();
	const auto conn_b = handler.OnOpen();
	BOOST_REQUIRE_NE(conn_a, conn_b);

	// OnOpen enqueues an initial SystemStateUpdate snapshot per connection; drain it.
	{
		auto seed_a = handler.DequeueMessage(conn_a);
		auto seed_b = handler.DequeueMessage(conn_b);
		BOOST_REQUIRE(seed_a.has_value());
		BOOST_REQUIRE(seed_b.has_value());
		auto seed_a_json = nlohmann::json::parse(*seed_a);
		BOOST_CHECK_EQUAL("SystemStateUpdate", seed_a_json["type"]);
	}

	// A single broadcast must be delivered to BOTH connections.
	DataHub().ORP(Kernel::ORP(650));

	auto msg_a = handler.DequeueMessage(conn_a);
	auto msg_b = handler.DequeueMessage(conn_b);
	BOOST_REQUIRE(msg_a.has_value());
	BOOST_REQUIRE(msg_b.has_value());

	// Both connections received an identical, fully-serialised payload.
	BOOST_CHECK_EQUAL(*msg_a, *msg_b);
	auto msg_a_json = nlohmann::json::parse(*msg_a);
	BOOST_CHECK_EQUAL("ChemistryUpdate", msg_a_json["type"]);
	BOOST_REQUIRE(msg_a_json["payload"].contains("orp"));
	BOOST_CHECK_EQUAL(650, msg_a_json["payload"]["orp"]);
}

BOOST_AUTO_TEST_CASE(Test_WebsocketRoutes_WsEvent_ConvertFromStringViewParsesWithoutCopy)
{
	const std::string_view valid_event{ R"({"type":"ChemistryUpdate","payload":{"orp":650}})" };

	auto parsed = HTTP::WebSocket_Event::ConvertFromStringView(valid_event);
	BOOST_REQUIRE(parsed.has_value());
	BOOST_CHECK_EQUAL(HTTP::WebSocket_EventTypes::ChemistryUpdate, parsed->Type());

	// Malformed JSON must fail gracefully (allow_exceptions=false path) rather than throw.
	const std::string_view invalid_event{ R"({not valid json)" };
	BOOST_CHECK_NO_THROW(HTTP::WebSocket_Event::ConvertFromStringView(invalid_event));
	BOOST_CHECK(!HTTP::WebSocket_Event::ConvertFromStringView(invalid_event).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
