#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>

#include "http/server/server_types.h"
#include "http/webroute_equipment_button.h"
#include "http/webroute_equipment_buttons.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/pool_configurations.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

//-----------------------------------------------------------------------------
// Regression coverage for the equipment-button routes' "status" member.
//
// A Light carries the very same AuxillaryStatusTrait the aux family uses (see
// LightsDevice, which writes it at construction, on every wire status message,
// and again on a watchdog timeout), and the routes already advertise a light as
// controllable:true. ResolveStatusString nonetheless lumped Light in with
// Unknown/default, so HasStatus() was false and BOTH button routes silently
// dropped the "status" member for every light -- a toggle the UI could not paint.
//-----------------------------------------------------------------------------

namespace
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	struct EquipmentButtonsFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		EquipmentButtonsFixture()
			: data_hub(Find<Kernel::DataHub>())
		{
			// The individual-button route reports 503 until the pool configuration
			// is known, so settle it before any request is made.
			data_hub->PoolConfiguration = Kernel::PoolConfigurations::SingleBody;
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

		std::shared_ptr<Kernel::DataHub> data_hub;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_EquipmentButtons, EquipmentButtonsFixture)

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

BOOST_AUTO_TEST_SUITE_END()
