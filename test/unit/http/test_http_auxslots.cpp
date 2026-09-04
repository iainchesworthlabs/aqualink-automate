#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_reconciliation.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/http/json/json_equipment_auxslots.h"
#include "jandy/http/webroute_equipment_auxslot.h"
#include "jandy/http/webroute_equipment_auxslots.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/preferences_hub.h"
#include "kernel/system_boards.h"
#include "options/options_preferences_options.h"
#include "preferences/preferences_service.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

namespace
{
	using enum Auxillaries::JandyAuxillaryIds;

	struct AuxSlotsFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		AuxSlotsFixture()
		{
			Options::Preferences::PreferencesSettings settings; // empty file -> in-memory only
			service = std::make_shared<Preferences::PreferencesService>(*this, settings);
		}

		std::shared_ptr<Kernel::DataHub> DataHub() { return Find<Kernel::DataHub>(); }
		std::shared_ptr<Kernel::PreferencesHub> PreferencesHub() { return Find<Kernel::PreferencesHub>(); }

		// Mimics an organically-detected aux: stable id, Jandy identity, label and a status.
		std::shared_ptr<Kernel::AuxillaryDevice> SeedAux(Auxillaries::JandyAuxillaryIds id, const std::string& label, std::optional<Kernel::AuxillaryStatuses> status = std::nullopt)
		{
			auto device = std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(id));
			device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
			device->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
			if (status.has_value())
			{
				device->AuxillaryTraits.Set(Traits::AuxillaryStatusTrait{}, status.value());
			}
			Auxillaries::EnsureAuxIdentity(device, id);
			DataHub()->Devices.Add(device);
			return device;
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

		// Collection route: GET /api/equipment/aux-slots
		HTTP::Response SendCollection(boost::beast::http::verb verb)
		{
			HTTP::WebRoute_Equipment_AuxSlots route(*this);
			return Exchange(route.OnRequest(MakeRequest(verb, HTTP::EQUIPMENTAUXSLOTS_ROUTE_URL, "")));
		}

		// Single-slot route: PUT /api/equipment/aux-slots/{aux_id}
		HTTP::Response SendSlot(boost::beast::http::verb verb, std::string_view target, const std::string& body, bool with_service = true)
		{
			HTTP::WebRoute_Equipment_AuxSlot route(*this, with_service ? service : nullptr);
			return Exchange(route.OnRequest(MakeRequest(verb, target, body)));
		}

		HTTP::Response Put(std::string_view aux_id_segment, const std::string& body, bool with_service = true)
		{
			return SendSlot(boost::beast::http::verb::put, std::string{ "/api/equipment/aux-slots/" } + std::string{ aux_id_segment }, body, with_service);
		}

		static nlohmann::json FindSlot(const nlohmann::json& slots, std::string_view aux_id)
		{
			for (const auto& slot : slots)
			{
				if (slot["aux_id"].get<std::string>() == aux_id)
				{
					return slot;
				}
			}
			return nlohmann::json{};
		}

		std::shared_ptr<Preferences::PreferencesService> service;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_AuxSlots, AuxSlotsFixture)

//-----------------------------------------------------------------------------
// GET /api/equipment/aux-slots (collection)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Collection_Get_ReturnsOneRowPerAddressableAuxId)
{
	auto resp = SendCollection(boost::beast::http::verb::get);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(body["slots"].is_array());
	// 7 + 8 + 8 + 8 numbered relays + Extra Aux.
	BOOST_CHECK_EQUAL(body["slots"].size(), 32u);

	const auto aux1 = FindSlot(body["slots"], "Aux1");
	BOOST_REQUIRE(!aux1.is_null());
	BOOST_CHECK_EQUAL(aux1["power_centre"].get<std::string>(), "A");
	BOOST_CHECK(aux1["in_model_span"].get<bool>());   // model unknown -> never trimmed
	BOOST_CHECK(!aux1["detected"].get<bool>());
	BOOST_CHECK_EQUAL(aux1["presence_override"].get<std::string>(), "auto");
	BOOST_CHECK(!aux1.contains("device_id"));
	BOOST_CHECK(!aux1.contains("label"));
	BOOST_CHECK(!aux1.contains("status"));

	const auto auxb3 = FindSlot(body["slots"], "Aux B3");
	BOOST_REQUIRE(!auxb3.is_null());
	BOOST_CHECK_EQUAL(auxb3["power_centre"].get<std::string>(), "B");

	// Extra Aux belongs to no numbered power centre.
	const auto extra = FindSlot(body["slots"], "Extra Aux");
	BOOST_REQUIRE(!extra.is_null());
	BOOST_CHECK(extra["power_centre"].is_null());
	BOOST_CHECK(extra["in_model_span"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Collection_Get_DetectedDevice_CarriesIdLabelStatusAndDisplayLabel)
{
	SeedAux(Aux_5, "Pool Light", Kernel::AuxillaryStatuses::On);
	SeedAux(Aux_6, "Waterfall"); // no status trait -> no "status" field

	PreferencesHub()->LabelOverrides["Pool Light"] = "Garden";
	PreferencesHub()->ShowAuxIdInLabel = true;

	const auto body = nlohmann::json::parse(SendCollection(boost::beast::http::verb::get).body());

	const auto aux5 = FindSlot(body["slots"], "Aux5");
	BOOST_REQUIRE(!aux5.is_null());
	BOOST_CHECK(aux5["detected"].get<bool>());
	BOOST_CHECK_EQUAL(aux5["device_id"].get<std::string>(), boost::uuids::to_string(Auxillaries::AuxStableId(Aux_5)));
	BOOST_CHECK_EQUAL(aux5["label"].get<std::string>(), "Pool Light");
	BOOST_CHECK_EQUAL(aux5["display_label"].get<std::string>(), "Garden (Aux5)");
	BOOST_CHECK_EQUAL(aux5["status"].get<std::string>(), "On");

	const auto aux6 = FindSlot(body["slots"], "Aux6");
	BOOST_REQUIRE(!aux6.is_null());
	BOOST_CHECK(aux6["detected"].get<bool>());
	BOOST_CHECK_EQUAL(aux6["label"].get<std::string>(), "Waterfall");
	BOOST_CHECK_EQUAL(aux6["display_label"].get<std::string>(), "Waterfall (Aux6)");
	BOOST_CHECK(!aux6.contains("status"));
}

BOOST_AUTO_TEST_CASE(Collection_Get_SurfacesPresentAndAbsentOverrides)
{
	PreferencesHub()->AuxPresenceOverrides["Aux6"] = "present";
	PreferencesHub()->AuxPresenceOverrides["Aux7"] = "absent";
	PreferencesHub()->AuxPresenceOverrides["Aux B1"] = "bogus"; // unrecognised -> auto

	const auto body = nlohmann::json::parse(SendCollection(boost::beast::http::verb::get).body());
	BOOST_CHECK_EQUAL(FindSlot(body["slots"], "Aux6")["presence_override"].get<std::string>(), "present");
	BOOST_CHECK_EQUAL(FindSlot(body["slots"], "Aux7")["presence_override"].get<std::string>(), "absent");
	BOOST_CHECK_EQUAL(FindSlot(body["slots"], "Aux B1")["presence_override"].get<std::string>(), "auto");
}

BOOST_AUTO_TEST_CASE(Collection_Get_SynthesizedDevice_IsNotReportedAsDetected)
{
	// A forced-present placeholder exists in the graph but the bus never confirmed it.
	auto device = SeedAux(Aux_5, "Aux5");
	device->AuxillaryTraits.Set(Auxillaries::SynthesizedTrait{}, true);

	const auto body = nlohmann::json::parse(SendCollection(boost::beast::http::verb::get).body());
	const auto aux5 = FindSlot(body["slots"], "Aux5");
	BOOST_CHECK(!aux5["detected"].get<bool>());
	BOOST_CHECK(aux5.contains("device_id")); // the placeholder is still addressable

	// Once real evidence clears the flag it counts as detected.
	device->AuxillaryTraits.Set(Auxillaries::SynthesizedTrait{}, false);
	const auto body2 = nlohmann::json::parse(SendCollection(boost::beast::http::verb::get).body());
	BOOST_CHECK(FindSlot(body2["slots"], "Aux5")["detected"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Collection_Get_KnownModel_TrimsSlotsOutsideTheSpan)
{
	// An identified single-centre RS-8: power centre B and beyond are outside the model.
	DataHub()->SystemBoard = Kernel::SystemBoards::RS8_Only;
	DataHub()->ExpectedPowerCenterCount = 1;
	DataHub()->ExpectedAuxillaryCount = 7;

	const auto body = nlohmann::json::parse(SendCollection(boost::beast::http::verb::get).body());
	BOOST_CHECK(FindSlot(body["slots"], "Aux1")["in_model_span"].get<bool>());
	BOOST_CHECK(FindSlot(body["slots"], "Aux7")["in_model_span"].get<bool>());
	BOOST_CHECK(!FindSlot(body["slots"], "Aux B1")["in_model_span"].get<bool>());
	BOOST_CHECK(!FindSlot(body["slots"], "Aux D8")["in_model_span"].get<bool>());
	BOOST_CHECK(FindSlot(body["slots"], "Extra Aux")["in_model_span"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Collection_Head_IsAccepted_Post_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, SendCollection(boost::beast::http::verb::head).result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendCollection(boost::beast::http::verb::post).result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendCollection(boost::beast::http::verb::put).result());
}

//-----------------------------------------------------------------------------
// PUT /api/equipment/aux-slots/{aux_id}
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Slot_NonPut_Returns405)
{
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendSlot(boost::beast::http::verb::get, "/api/equipment/aux-slots/Aux5", "").result());
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, SendSlot(boost::beast::http::verb::post, "/api/equipment/aux-slots/Aux5", R"({"presence_override":"present"})").result());
}

BOOST_AUTO_TEST_CASE(Slot_Put_NoPreferencesService_Returns503)
{
	auto resp = Put("Aux5", R"({"presence_override":"present"})", /*with_service=*/false);
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "prefs_hub_unavailable");
}

BOOST_AUTO_TEST_CASE(Slot_Put_InvalidAuxId_Returns404WithTheOffendingId)
{
	auto resp = Put("Aux99", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "invalid_aux_id");
	BOOST_CHECK_EQUAL(body["params"]["aux_id"].get<std::string>(), "Aux99");

	// Not an aux at all.
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, Put("Pool%20Light", R"({"presence_override":"present"})").result());
}

BOOST_AUTO_TEST_CASE(Slot_Put_MissingIdSegment_Returns404)
{
	// Only three path segments -> no aux id to extract.
	auto resp = SendSlot(boost::beast::http::verb::put, "/api/equipment/aux-slots", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "invalid_aux_id");
	BOOST_CHECK_EQUAL(body["params"]["aux_id"].get<std::string>(), "");
}

BOOST_AUTO_TEST_CASE(Slot_Put_InvalidJson_Returns400)
{
	auto resp = Put("Aux5", "{ nope");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "invalid_json");
}

BOOST_AUTO_TEST_CASE(Slot_Put_MissingOrNonStringPresenceOverride_Returns400)
{
	for (const auto* payload : { R"({})", R"({"presence_override": true})", R"({"presence": "present"})" })
	{
		auto resp = Put("Aux5", payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "presence_override_required");
	}
	BOOST_CHECK(PreferencesHub()->AuxPresenceOverrides.empty());
}

BOOST_AUTO_TEST_CASE(Slot_Put_UnknownPresenceOverrideValue_Returns400)
{
	auto resp = Put("Aux5", R"({"presence_override":"maybe"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "invalid_presence_override");
	BOOST_CHECK(PreferencesHub()->AuxPresenceOverrides.empty());
}

BOOST_AUTO_TEST_CASE(Slot_Put_ForcePresent_SynthesizesDeviceAndPersistsOverride)
{
	BOOST_REQUIRE(nullptr == DataHub()->Devices.FindById(Auxillaries::AuxStableId(Aux_5)));

	auto resp = Put("Aux5", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	// Persisted through the preferences pipeline ...
	BOOST_REQUIRE(PreferencesHub()->AuxPresenceOverrides.contains("Aux5"));
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides["Aux5"].get<std::string>(), "present");

	// ... and reconciled into the device graph in the same request.
	const auto device = DataHub()->Devices.FindById(Auxillaries::AuxStableId(Aux_5));
	BOOST_REQUIRE(nullptr != device);

	// The response is the single updated row: forced but not yet detected.
	const auto slot = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(slot["aux_id"].get<std::string>(), "Aux5");
	BOOST_CHECK_EQUAL(slot["presence_override"].get<std::string>(), "present");
	BOOST_CHECK(!slot["detected"].get<bool>());
	BOOST_CHECK_EQUAL(slot["device_id"].get<std::string>(), boost::uuids::to_string(device->Id()));
}

BOOST_AUTO_TEST_CASE(Slot_Put_ForceAbsent_RemovesDetectedDevice)
{
	SeedAux(Aux_6, "Waterfall", Kernel::AuxillaryStatuses::Off);
	BOOST_REQUIRE(nullptr != DataHub()->Devices.FindById(Auxillaries::AuxStableId(Aux_6)));

	auto resp = Put("Aux6", R"({"presence_override":"absent"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	BOOST_CHECK(nullptr == DataHub()->Devices.FindById(Auxillaries::AuxStableId(Aux_6)));
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides["Aux6"].get<std::string>(), "absent");

	const auto slot = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(slot["aux_id"].get<std::string>(), "Aux6");
	BOOST_CHECK_EQUAL(slot["presence_override"].get<std::string>(), "absent");
	BOOST_CHECK(!slot["detected"].get<bool>());
	BOOST_CHECK(!slot.contains("device_id"));
}

BOOST_AUTO_TEST_CASE(Slot_Put_Auto_ClearsTheOverrideKey)
{
	PreferencesHub()->AuxPresenceOverrides["Aux5"] = "present";
	PreferencesHub()->AuxPresenceOverrides["Aux6"] = "absent";

	auto resp = Put("Aux5", R"({"presence_override":"auto"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	BOOST_CHECK(!PreferencesHub()->AuxPresenceOverrides.contains("Aux5"));
	// Unrelated entries survive the merge.
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides["Aux6"].get<std::string>(), "absent");

	const auto slot = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(slot["presence_override"].get<std::string>(), "auto");
}

BOOST_AUTO_TEST_CASE(Slot_Put_UrlEncodedBankId_CanonicalisesTheKey)
{
	// "Aux%20B1" -> "Aux B1"; the stored key is the enum's canonical spelling.
	auto resp = Put("Aux%20B1", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE(PreferencesHub()->AuxPresenceOverrides.contains("Aux B1"));
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["aux_id"].get<std::string>(), "Aux B1");

	// The no-space spelling collapses onto the SAME key rather than creating a second one.
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, Put("AuxB1", R"({"presence_override":"absent"})").result());
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides.size(), 1u);
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides["Aux B1"].get<std::string>(), "absent");
}

BOOST_AUTO_TEST_CASE(Slot_Put_NonObjectStoredOverrides_AreReplacedByAFreshObject)
{
	// Defensive: a corrupt in-memory blob must not break the merge.
	PreferencesHub()->AuxPresenceOverrides = nlohmann::json::array();

	auto resp = Put("Aux5", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE(PreferencesHub()->AuxPresenceOverrides.is_object());
	BOOST_CHECK_EQUAL(PreferencesHub()->AuxPresenceOverrides["Aux5"].get<std::string>(), "present");
}

BOOST_AUTO_TEST_CASE(Slot_Put_PreferencesServiceRejectsMergedBlob_Returns400)
{
	// The merge carries every existing entry through ApplyJson's validator; a pre-existing
	// malformed value therefore fails validation and nothing is applied.
	PreferencesHub()->AuxPresenceOverrides["Aux1"] = "bogus";

	auto resp = Put("Aux5", R"({"presence_override":"present"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "invalid_aux_presence_overrides");
	BOOST_CHECK(!PreferencesHub()->AuxPresenceOverrides.contains("Aux5"));
	BOOST_CHECK(nullptr == DataHub()->Devices.FindById(Auxillaries::AuxStableId(Aux_5)));
}

//-----------------------------------------------------------------------------
// GenerateJson_Equipment_AuxSlots called directly (label-override plumbing)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GenerateJson_NoOverridesNoAuxId_DisplayLabelIsCanonical)
{
	SeedAux(Aux_3, "Spa Light", Kernel::AuxillaryStatuses::Off);

	const auto slots = HTTP::JSON::GenerateJson_Equipment_AuxSlots(DataHub(), nlohmann::json::object(), false, nlohmann::json::object());
	const auto aux3 = FindSlot(slots, "Aux3");
	BOOST_REQUIRE(!aux3.is_null());
	BOOST_CHECK_EQUAL(aux3["display_label"].get<std::string>(), "Spa Light");
	BOOST_CHECK_EQUAL(aux3["status"].get<std::string>(), "Off");
}

BOOST_AUTO_TEST_SUITE_END()
