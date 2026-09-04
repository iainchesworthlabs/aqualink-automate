#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_equipment.h"
#include "http/webroute_equipment_button.h"
#include "http/webroute_equipment_buttons.h"
#include "interfaces/icommanddispatcher.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_validation.h"
#include "kernel/orp.h"
#include "kernel/ph.h"
#include "kernel/pool_configurations.h"
#include "kernel/preferences_hub.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

namespace
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	// Recording dispatcher: captures the UUID of every toggle and returns a configurable result
	// so each CommandResult -> HTTP mapping of the single-button route can be driven.
	class StubCommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		CommandResult result_to_return{ CommandResult::Success };
		std::vector<boost::uuids::uuid> toggled;

		CommandResult ToggleByUuid(const boost::uuids::uuid& uuid) override { toggled.push_back(uuid); return result_to_return; }
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
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule&, const Scheduling::ControllerSchedule&) override { return CommandResult::Success; }
	};

	struct EquipmentButtonsFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		EquipmentButtonsFixture()
			: data_hub(Find<Kernel::DataHub>())
			, dispatcher(std::make_shared<StubCommandDispatcher>())
		{
		}

		// Adds a device of `type` carrying `status` in the aux status trait, and
		// returns its (stable, label-derived) UUID.
		boost::uuids::uuid AddDevice(AuxillaryTypes type, const std::string& label, Kernel::AuxillaryStatuses status)
		{
			auto device = std::make_shared<Kernel::AuxillaryDevice>(label);
			device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, type);
			device->AuxillaryTraits.Set(LabelTrait{}, label);
			device->AuxillaryTraits.Set(AuxillaryStatusTrait{}, status);

			const auto id = device->Id();
			data_hub->Devices.Add(std::move(device));
			return id;
		}

		// Button POST resolves the dispatcher in the route constructor; the "no dispatcher"
		// 503 is exercised by simply not calling this.
		void RegisterDispatcher()
		{
			Register(std::static_pointer_cast<Interfaces::ICommandDispatcher>(dispatcher));
		}

		std::shared_ptr<Kernel::DataHub> DataHub() { return Find<Kernel::DataHub>(); }
		std::shared_ptr<Kernel::PreferencesHub> PreferencesHub() { return Find<Kernel::PreferencesHub>(); }

		// The individual-button route reports 503 until the pool configuration is known;
		// call this before any request that needs the system considered initialised.
		void MarkSystemInitialised()
		{
			DataHub()->PoolConfiguration = Kernel::PoolConfigurations::SingleBody;
		}

		// Seeds a device the way the Jandy aux factory does: type + canonical label, optionally
		// the protocol-native hardware label ("Aux5") and an AuxillaryStatusTrait.
		//
		// NOTE: a status only reaches the JSON when the device's *type* maps to the status trait
		// that carries it (Kernel::AuxillaryTraitsTypes::ResolveStatusString): AuxillaryStatusTrait
		// is consulted for Auxillary/Cleaner/Light/Spillover/Sprinkler. Unknown-type devices have
		// no status mapping at all, so seeding one of those with with_status = true still yields a
		// button with no "status" member -- use Auxillary (or Light) when asserting on status.
		std::shared_ptr<Kernel::AuxillaryDevice> SeedDevice(const std::string& label, Traits::AuxillaryTypes type, bool with_status = true, const std::string& hardware_id = {})
		{
			auto device = std::make_shared<Kernel::AuxillaryDevice>();
			device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, type);
			device->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
			if (!hardware_id.empty())
			{
				device->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, hardware_id);
			}
			if (with_status)
			{
				device->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
			}
			DataHub()->Devices.Add(device);
			return device;
		}

		// Round-trips a route's response through a Beast stream pair so the test
		// reads exactly the bytes a client would (mirrors the chlorinator tests).
		static HTTP::Response Transact(HTTP::Message&& msg)
		{
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

		static HTTP::Request MakeGet(const std::string& target)
		{
			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::get);
			req.target(target);
			req.set(boost::beast::http::field::host, "localhost.localdomain");
			req.set(boost::beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
			req.prepare_payload();
			return req;
		}

		nlohmann::json GetCollection()
		{
			HTTP::WebRoute_Equipment_Buttons route(*this);
			auto resp = Transact(route.OnRequest(MakeGet(HTTP::EQUIPMENTBUTTONS_ROUTE_URL)));
			BOOST_REQUIRE_EQUAL(boost::beast::http::status::ok, resp.result());
			return nlohmann::json::parse(resp.body());
		}

		nlohmann::json GetIndividual(const boost::uuids::uuid& id)
		{
			HTTP::WebRoute_Equipment_Button route(*this);
			const auto target = std::string("/api/equipment/buttons/") + boost::uuids::to_string(id);
			auto resp = Transact(route.OnRequest(MakeGet(target)));
			BOOST_REQUIRE_EQUAL(boost::beast::http::status::ok, resp.result());
			return nlohmann::json::parse(resp.body());
		}

		// Locates one button in the collection response by its UUID.
		static nlohmann::json FindButton(const nlohmann::json& collection, const boost::uuids::uuid& id)
		{
			BOOST_REQUIRE(collection.contains("buttons"));
			const auto id_str = boost::uuids::to_string(id);
			for (const auto& button : collection["buttons"])
			{
				if (button.contains("id") && button["id"] == id_str)
				{
					return button;
				}
			}
			BOOST_FAIL("Button '" << id_str << "' was not present in the collection response");
			return nlohmann::json::object();
		}

		static HTTP::Request MakeRequest(boost::beast::http::verb verb, std::string_view target, const std::string& body)
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

		static HTTP::Response Exchange(HTTP::Message&& msg)
		{
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

		HTTP::Response SendEquipment(boost::beast::http::verb verb)
		{
			HTTP::WebRoute_Equipment route(*this);
			return Exchange(route.OnRequest(MakeRequest(verb, HTTP::EQUIPMENT_ROUTE_URL, "")));
		}

		HTTP::Response SendButtons(boost::beast::http::verb verb)
		{
			HTTP::WebRoute_Equipment_Buttons route(*this);
			return Exchange(route.OnRequest(MakeRequest(verb, HTTP::EQUIPMENTBUTTONS_ROUTE_URL, "")));
		}

		HTTP::Response SendButton(boost::beast::http::verb verb, std::string_view target)
		{
			HTTP::WebRoute_Equipment_Button route(*this);
			return Exchange(route.OnRequest(MakeRequest(verb, target, "")));
		}

		HTTP::Response GetButton(const std::string& id) { return SendButton(boost::beast::http::verb::get, "/api/equipment/buttons/" + id); }
		HTTP::Response PostButton(const std::string& id) { return SendButton(boost::beast::http::verb::post, "/api/equipment/buttons/" + id); }

		// Locates one button in an already-unwrapped `buttons` array by its label.
		static nlohmann::json FindButton(const nlohmann::json& buttons, std::string_view label)
		{
			for (const auto& button : buttons)
			{
				if (button.contains("label") && (button["label"].get<std::string>() == label))
				{
					return button;
				}
			}
			return nlohmann::json{};
		}

		std::shared_ptr<Kernel::DataHub> data_hub;
		std::shared_ptr<StubCommandDispatcher> dispatcher;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_EquipmentButtons, EquipmentButtonsFixture)

//-----------------------------------------------------------------------------
// Regression coverage for the equipment-button routes' "status" member.
//
// A Light carries the very same AuxillaryStatusTrait the aux family uses (see
// LightsDevice, which writes it at construction, on every wire status message,
// and again on a watchdog timeout), and the routes report a light as not
// controllable (a separate colour-light controller, not the relay that drives
// it). ResolveStatusString nonetheless used to lump Light in with Unknown/default,
// so HasStatus() was false and BOTH button routes silently dropped the "status"
// member for every light -- a state the UI could not paint.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GetCollection_LightReportsItsStatus)
{
	const auto light = AddDevice(AuxillaryTypes::Light, "Light 0x08", Kernel::AuxillaryStatuses::On);

	const auto button = FindButton(GetCollection(), light);

	BOOST_REQUIRE_MESSAGE(button.contains("status"), "A light must report its on/off state to the API");
	BOOST_CHECK_EQUAL(button["status"].get<std::string>(), std::string("On"));
	BOOST_CHECK_EQUAL(button["device_type"].get<std::string>(), std::string("Light"));
}

//-----------------------------------------------------------------------------
// A light reports its state but is NOT controllable.
//
// A Light is a separate RS-485 colour-light controller, not the aux relay that
// switches it (that relay is a distinct Auxillary device in this same list, and is
// genuinely controllable). A light carries no hardware aux id and only a synthetic
// label, so every actuation path reports MappingFailed and the dispatcher returns
// UnknownEquipmentType -> HTTP 422. Advertising a toggle that can only ever 422 is
// worse than advertising none, so `controllable` must be false.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GetCollection_LightIsNotControllable)
{
	const auto light = AddDevice(AuxillaryTypes::Light, "Light 0xf0", Kernel::AuxillaryStatuses::On);

	const auto button = FindButton(GetCollection(), light);

	BOOST_REQUIRE(button.contains("controllable"));
	BOOST_CHECK_MESSAGE(!button["controllable"].get<bool>(),
		"A light must not be advertised as a toggle - actuating one always returns 422");

	// It is read-only, not invisible: the state it reports is the whole point.
	BOOST_REQUIRE(button.contains("status"));
	BOOST_CHECK_EQUAL(button["status"].get<std::string>(), std::string("On"));
}

BOOST_AUTO_TEST_CASE(GetCollection_AuxillaryFamilyRemainsControllable)
{
	// Excluding Light must not have made the rest of the aux family read-only -- an
	// aux relay (including the one that actually drives a pool light) stays a toggle.
	const auto aux = AddDevice(AuxillaryTypes::Auxillary, "Pool Light", Kernel::AuxillaryStatuses::On);
	const auto cleaner = AddDevice(AuxillaryTypes::Cleaner, "Cleaner", Kernel::AuxillaryStatuses::Off);

	const auto collection = GetCollection();

	BOOST_CHECK_EQUAL(FindButton(collection, aux)["controllable"].get<bool>(), true);
	BOOST_CHECK_EQUAL(FindButton(collection, cleaner)["controllable"].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(GetIndividual_LightReportsItsStatus)
{
	MarkSystemInitialised();
	const auto light = AddDevice(AuxillaryTypes::Light, "Light 0x09", Kernel::AuxillaryStatuses::Off);

	const auto button = GetIndividual(light);

	BOOST_REQUIRE_MESSAGE(button.contains("status"), "A light must report its on/off state to the API");
	BOOST_CHECK_EQUAL(button["status"].get<std::string>(), std::string("Off"));
}

BOOST_AUTO_TEST_CASE(GetCollection_LightStatusTracksTheTrait)
{
	// The trait is what the wire path writes; the route must reflect each value it
	// can take, including the Unknown a watchdog timeout parks the light on.
	const auto light = AddDevice(AuxillaryTypes::Light, "Light 0x0a", Kernel::AuxillaryStatuses::On);
	auto device = data_hub->Devices.FindById(light);
	BOOST_REQUIRE(nullptr != device);

	BOOST_CHECK_EQUAL(FindButton(GetCollection(), light)["status"].get<std::string>(), std::string("On"));

	device->AuxillaryTraits.Set(AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
	BOOST_CHECK_EQUAL(FindButton(GetCollection(), light)["status"].get<std::string>(), std::string("Off"));

	device->AuxillaryTraits.Set(AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Unknown);
	BOOST_CHECK_EQUAL(FindButton(GetCollection(), light)["status"].get<std::string>(), std::string("Unknown"));
}

BOOST_AUTO_TEST_CASE(GetCollection_AuxillaryFamilyStillReportsStatus)
{
	// The widened arm must not have disturbed the types that already worked.
	const auto aux = AddDevice(AuxillaryTypes::Auxillary, "Aux1", Kernel::AuxillaryStatuses::On);
	const auto cleaner = AddDevice(AuxillaryTypes::Cleaner, "Cleaner", Kernel::AuxillaryStatuses::Off);
	const auto spillover = AddDevice(AuxillaryTypes::Spillover, "Spillover", Kernel::AuxillaryStatuses::Enabled);
	const auto sprinkler = AddDevice(AuxillaryTypes::Sprinkler, "Sprinkler", Kernel::AuxillaryStatuses::Pending);

	const auto collection = GetCollection();

	BOOST_CHECK_EQUAL(FindButton(collection, aux)["status"].get<std::string>(), std::string("On"));
	BOOST_CHECK_EQUAL(FindButton(collection, cleaner)["status"].get<std::string>(), std::string("Off"));
	BOOST_CHECK_EQUAL(FindButton(collection, spillover)["status"].get<std::string>(), std::string("Enabled"));
	BOOST_CHECK_EQUAL(FindButton(collection, sprinkler)["status"].get<std::string>(), std::string("Pending"));
}

BOOST_AUTO_TEST_CASE(GetCollection_UnknownTypeStillOmitsStatus)
{
	// Only Unknown-type devices carry no status member: the fix widened the arm,
	// it did not turn "status" into an unconditional field.
	const auto unknown = AddDevice(AuxillaryTypes::Unknown, "Mystery Device", Kernel::AuxillaryStatuses::On);

	const auto button = FindButton(GetCollection(), unknown);

	BOOST_CHECK(!button.contains("status"));
	BOOST_CHECK_EQUAL(button["controllable"].get<bool>(), false);
}

//-----------------------------------------------------------------------------
// GET /api/equipment -- the branches the OneTouch-driven tests never reach
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Equipment_Get_ValidationResultAndSensorsAreSurfaced)
{
	Kernel::EquipmentValidation validation;
	validation.ExpectedAuxillaries = 7;
	validation.DiscoveredAuxillaries = 6;
	validation.ExpectedPowerCenters = 1;
	validation.DiscoveredPowerCenters = 1;
	validation.Anomalies = { "Aux7 not discovered" };
	DataHub()->EquipmentValidationResult = validation;

	DataHub()->PoolHeater2Enabled(true);
	DataHub()->ORP(Kernel::ORP{ 650.0 });
	DataHub()->pH(Kernel::pH{ 7.4f });

	auto resp = SendEquipment(boost::beast::http::verb::get);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	const auto body = nlohmann::json::parse(resp.body());

	const auto& v = body["configuration"]["validation"];
	BOOST_REQUIRE(!v.is_null());
	BOOST_CHECK(!v["passed"].get<bool>());
	BOOST_CHECK_EQUAL(v["expected_auxillaries"].get<int>(), 7);
	BOOST_CHECK_EQUAL(v["discovered_auxillaries"].get<int>(), 6);
	BOOST_CHECK_EQUAL(v["expected_power_centers"].get<int>(), 1);
	BOOST_CHECK_EQUAL(v["discovered_power_centers"].get<int>(), 1);
	BOOST_REQUIRE_EQUAL(v["anomalies"].size(), 1u);
	BOOST_CHECK_EQUAL(v["anomalies"][0].get<std::string>(), "Aux7 not discovered");

	BOOST_CHECK(body["temperatures"]["pool_heater_2_enabled"].get<bool>());
	BOOST_CHECK_EQUAL(body["chemistry"]["orp_mv"].get<int>(), 650);
	BOOST_CHECK_CLOSE(body["chemistry"]["ph"].get<double>(), 7.4, 0.01);
}

BOOST_AUTO_TEST_CASE(Equipment_Get_CleanValidationPasses)
{
	DataHub()->EquipmentValidationResult = Kernel::EquipmentValidation{};
	const auto body = nlohmann::json::parse(SendEquipment(boost::beast::http::verb::get).body());
	BOOST_CHECK(body["configuration"]["validation"]["passed"].get<bool>());
	BOOST_CHECK(body["configuration"]["validation"]["anomalies"].empty());
}

BOOST_AUTO_TEST_CASE(Equipment_Get_BodiesIncludeANonPoolSpaBody)
{
	DataHub()->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment);
	// A body whose id has no dedicated temperature channel takes the default arm.
	DataHub()->AddBody(Kernel::BodyOfWater{ Kernel::BodyOfWaterIds::Shared, "Shared" });

	const auto body = nlohmann::json::parse(SendEquipment(boost::beast::http::verb::get).body());
	const auto& bodies = body["configuration"]["bodies"];
	BOOST_REQUIRE_EQUAL(bodies.size(), 3u);
	BOOST_CHECK_EQUAL(bodies[0]["id"].get<std::string>(), "Pool");
	BOOST_CHECK(bodies[0]["is_active"].get<bool>());
	BOOST_CHECK_EQUAL(bodies[1]["id"].get<std::string>(), "Spa");
	BOOST_CHECK_EQUAL(bodies[2]["id"].get<std::string>(), "Shared");
	BOOST_CHECK_EQUAL(bodies[2]["label"].get<std::string>(), "Shared");
	BOOST_CHECK(bodies[2]["temperature"].is_null());
	BOOST_CHECK(bodies[2]["setpoint"].is_null());
	BOOST_CHECK_EQUAL(body["configuration"]["pool_configuration"].get<std::string>(), "DualBody_SharedEquipment");
}

BOOST_AUTO_TEST_CASE(Equipment_Get_LabelOverridesFlowIntoDevices)
{
	SeedDevice("Aux1", Traits::AuxillaryTypes::Auxillary);
	PreferencesHub()->LabelOverrides["Aux1"] = "Pool Light";
	PreferencesHub()->ShowAuxIdInLabel = true;

	const auto body = nlohmann::json::parse(SendEquipment(boost::beast::http::verb::get).body());
	const auto& auxes = body["devices"]["auxillaries"];
	BOOST_REQUIRE(auxes.is_array());
	BOOST_REQUIRE_EQUAL(auxes.size(), 1u);
	BOOST_CHECK_EQUAL(auxes[0]["label"].get<std::string>(), "Aux1");
	BOOST_CHECK_EQUAL(auxes[0]["display_label"].get<std::string>(), "Pool Light");
}

//-----------------------------------------------------------------------------
// GET/POST /api/equipment/buttons (collection)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Buttons_Get_HardwareIdAndControllabilityPerDeviceType)
{
	SeedDevice("Pool Light", Traits::AuxillaryTypes::Auxillary, true, "Aux5");
	SeedDevice("Spa Light", Traits::AuxillaryTypes::Light, true, "Aux6");
	SeedDevice("AquaPure", Traits::AuxillaryTypes::Chlorinator, false);
	SeedDevice("Mystery", Traits::AuxillaryTypes::Unknown, false);

	PreferencesHub()->LabelOverrides["Pool Light"] = "Garden";
	PreferencesHub()->ShowAuxIdInLabel = true;

	auto resp = SendButtons(boost::beast::http::verb::get);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	const auto body = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE_EQUAL(body["buttons"].size(), 4u);

	const auto aux = FindButton(body["buttons"], "Pool Light");
	BOOST_REQUIRE(!aux.is_null());
	BOOST_CHECK_EQUAL(aux["hardware_id"].get<std::string>(), "Aux5");
	BOOST_CHECK_EQUAL(aux["display_label"].get<std::string>(), "Garden (Aux5)");
	BOOST_CHECK_EQUAL(aux["status"].get<std::string>(), "Off");
	BOOST_CHECK_EQUAL(aux["device_type"].get<std::string>(), "Auxillary");
	BOOST_CHECK(aux["controllable"].get<bool>());

	// A light is NOT controllable -- it is a separate RS-485 colour-light controller, not the
	// aux relay that switches it (that relay is a distinct, controllable Auxillary device, e.g.
	// the "Pool Light" aux above). It falls back to "canonical (Aux id)" (having no override),
	// and it DOES report its status, via the very same AuxillaryStatusTrait the aux family uses.
	const auto light = FindButton(body["buttons"], "Spa Light");
	BOOST_REQUIRE(!light.is_null());
	BOOST_CHECK_EQUAL(light["hardware_id"].get<std::string>(), "Aux6");
	BOOST_CHECK_EQUAL(light["display_label"].get<std::string>(), "Spa Light (Aux6)");
	BOOST_CHECK_EQUAL(light["device_type"].get<std::string>(), "Light");
	BOOST_CHECK(!light["controllable"].get<bool>());
	BOOST_REQUIRE(light.contains("status"));
	BOOST_CHECK_EQUAL(light["status"].get<std::string>(), "Off");

	const auto swg = FindButton(body["buttons"], "AquaPure");
	BOOST_REQUIRE(!swg.is_null());
	BOOST_CHECK(!swg.contains("hardware_id"));
	BOOST_CHECK(!swg.contains("status"));
	BOOST_CHECK(!swg["controllable"].get<bool>());

	const auto unknown = FindButton(body["buttons"], "Mystery");
	BOOST_REQUIRE(!unknown.is_null());
	BOOST_CHECK(!unknown["controllable"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Buttons_Post_ReturnsEmptyOk)
{
	auto resp = SendButtons(boost::beast::http::verb::post);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK(resp.body().empty());
}

BOOST_AUTO_TEST_CASE(Buttons_Put_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendButtons(boost::beast::http::verb::put).result());
}

//-----------------------------------------------------------------------------
// GET /api/equipment/buttons/{button_id}
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Button_Delete_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendButton(boost::beast::http::verb::delete_, "/api/equipment/buttons/x").result());
}

BOOST_AUTO_TEST_CASE(Button_Get_SystemNotInitialised_Returns503WithRetryAfter)
{
	// PoolConfiguration is Unknown until the startup scrape completes.
	auto resp = GetButton("00000000-0000-0000-0000-000000000000");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK(!resp[boost::beast::http::field::retry_after].empty());
}

BOOST_AUTO_TEST_CASE(Button_Get_MissingId_Returns404)
{
	MarkSystemInitialised();
	// Three path segments and no query -> no id at all.
	auto resp = SendButton(boost::beast::http::verb::get, "/api/equipment/buttons");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(resp.body().find("Unknown Or Missing Button Id") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Button_Get_MalformedUuid_Returns404)
{
	MarkSystemInitialised();
	auto resp = GetButton("not-a-uuid");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(resp.body().find("Unknown Or Missing Button Id") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Button_Get_UnknownUuid_Returns404WithTheId)
{
	MarkSystemInitialised();
	const std::string id{ "12345678-1234-1234-1234-123456789abc" };
	auto resp = GetButton(id);
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(resp.body().find(id) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Button_Get_KnownDevice_ReturnsIdLabelStatus)
{
	MarkSystemInitialised();
	// Auxillary (not Light): only the aux family resolves AuxillaryStatusTrait into "status".
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Auxillary);
	const auto id = boost::uuids::to_string(device->Id());

	auto resp = GetButton(id);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["id"].get<std::string>(), id);
	BOOST_CHECK_EQUAL(body["label"].get<std::string>(), "Pool Light");
	BOOST_CHECK_EQUAL(body["status"].get<std::string>(), "Off");
}

BOOST_AUTO_TEST_CASE(Button_Get_DeviceWithoutStatus_OmitsStatus)
{
	MarkSystemInitialised();
	const auto device = SeedDevice("Mystery", Traits::AuxillaryTypes::Unknown, false);

	const auto body = nlohmann::json::parse(GetButton(boost::uuids::to_string(device->Id())).body());
	BOOST_CHECK_EQUAL(body["label"].get<std::string>(), "Mystery");
	BOOST_CHECK(!body.contains("status"));
}

BOOST_AUTO_TEST_CASE(Button_Get_QueryStringFallback_ResolvesTheDevice)
{
	MarkSystemInitialised();
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);
	const auto id = boost::uuids::to_string(device->Id());

	auto resp = SendButton(boost::beast::http::verb::get, "/api/equipment/buttons?button_id=" + id);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["id"].get<std::string>(), id);
}

//-----------------------------------------------------------------------------
// POST /api/equipment/buttons/{button_id}
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Button_Post_SystemNotInitialised_Returns503)
{
	RegisterDispatcher();
	auto resp = PostButton("12345678-1234-1234-1234-123456789abc");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK(dispatcher->toggled.empty());
}

BOOST_AUTO_TEST_CASE(Button_Post_MalformedUuid_Returns404)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, PostButton("garbage").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, SendButton(boost::beast::http::verb::post, "/api/equipment/buttons").result());
	BOOST_CHECK(dispatcher->toggled.empty());
}

BOOST_AUTO_TEST_CASE(Button_Post_UnknownUuid_Returns404)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, PostButton("12345678-1234-1234-1234-123456789abc").result());
	BOOST_CHECK(dispatcher->toggled.empty());
}

BOOST_AUTO_TEST_CASE(Button_Post_NoDispatcher_Returns503)
{
	MarkSystemInitialised();
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);
	auto resp = PostButton(boost::uuids::to_string(device->Id()));
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
}

BOOST_AUTO_TEST_CASE(Button_Post_Success_TogglesAndReportsCommand)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	// Auxillary (not Light): only the aux family resolves AuxillaryStatusTrait into "status".
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Auxillary);
	const auto id = boost::uuids::to_string(device->Id());

	auto resp = PostButton(id);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(dispatcher->toggled.size(), 1u);
	BOOST_CHECK(dispatcher->toggled.front() == device->Id());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["id"].get<std::string>(), id);
	BOOST_CHECK_EQUAL(body["label"].get<std::string>(), "Pool Light");
	BOOST_CHECK_EQUAL(body["status"].get<std::string>(), "Off");
	BOOST_CHECK_EQUAL(body["command"].get<std::string>(), "toggled");
}

BOOST_AUTO_TEST_CASE(Button_Post_Success_DeviceWithoutStatus_OmitsStatus)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	const auto device = SeedDevice("Mystery", Traits::AuxillaryTypes::Auxillary, false);

	const auto body = nlohmann::json::parse(PostButton(boost::uuids::to_string(device->Id())).body());
	BOOST_CHECK_EQUAL(body["command"].get<std::string>(), "toggled");
	BOOST_CHECK(!body.contains("status"));
}

BOOST_AUTO_TEST_CASE(Button_Post_NoSerialAdapter_Returns503)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter;
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);

	auto resp = PostButton(boost::uuids::to_string(device->Id()));
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK(!resp[boost::beast::http::field::retry_after].empty());
}

BOOST_AUTO_TEST_CASE(Button_Post_DeviceNotFound_Returns404)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound;
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);
	const auto id = boost::uuids::to_string(device->Id());

	auto resp = PostButton(id);
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK(resp.body().find(id) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Button_Post_UnknownEquipmentType_Returns422)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType;
	const auto device = SeedDevice("Mystery", Traits::AuxillaryTypes::Unknown);

	BOOST_CHECK_EQUAL(boost::beast::http::status::unprocessable_entity, PostButton(boost::uuids::to_string(device->Id())).result());
}

BOOST_AUTO_TEST_CASE(Button_Post_InvalidValue_Returns400)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::InvalidValue;
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, PostButton(boost::uuids::to_string(device->Id())).result());
}

BOOST_AUTO_TEST_CASE(Button_Post_Busy_Returns409WithRetryAfter)
{
	RegisterDispatcher();
	MarkSystemInitialised();
	dispatcher->result_to_return = Interfaces::ICommandDispatcher::CommandResult::Busy;
	const auto device = SeedDevice("Pool Light", Traits::AuxillaryTypes::Light);

	auto resp = PostButton(boost::uuids::to_string(device->Id()));
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
	BOOST_CHECK(!resp[boost::beast::http::field::retry_after].empty());
	BOOST_REQUIRE_EQUAL(dispatcher->toggled.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()
