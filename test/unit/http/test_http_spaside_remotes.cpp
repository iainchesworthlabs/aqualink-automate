#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
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
#include "http/webroute_equipment_spaside_remotes.h"
#include "interfaces/ispasideremotecontroller.h"
#include "kernel/data_hub.h"
#include "kernel/preferences_hub.h"
#include "options/options_preferences_options.h"
#include "preferences/preferences_service.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	// Recording spa-side controller: serves a configurable remote snapshot / function list and
	// returns configurable press/assign outcomes so every result->status mapping in the route can
	// be driven without a Jandy stack.
	class StubSpasideController : public Interfaces::ISpasideRemoteController
	{
	public:
		std::vector<RemoteState> remotes;
		std::vector<std::string> available_functions;
		PressResult press_result{ PressResult::Success };
		AssignResult assign_result{ AssignResult::Accepted };

		std::vector<std::pair<uint8_t, uint8_t>> presses;
		std::vector<std::tuple<uint8_t, uint8_t, std::string>> assigns;

		std::vector<RemoteState> Remotes() const override { return remotes; }
		PressResult PressButton(uint8_t address, uint8_t button_index) override { presses.emplace_back(address, button_index); return press_result; }
		AssignResult SetButtonAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function) override { assigns.emplace_back(switch_number, button_number, function); return assign_result; }
		std::vector<std::string> AvailableFunctions() const override { return available_functions; }
	};

	Interfaces::ISpasideRemoteController::RemoteState MakeDualSpaSwitch()
	{
		Interfaces::ISpasideRemoteController::RemoteState remote;
		remote.address = 0x10;
		remote.device_class = "DualSpaSwitch";
		remote.emulated = true;
		remote.button_count = 3;
		remote.poll_count = 42;
		remote.last_button = 2;
		remote.led_image_seen = true;
		remote.leds = { "on", "off", "blink" };
		remote.led_image = "a5ff";
		remote.buttons = {
			{ .index = 1, .switch_number = 1, .button_number = 1, .assignable = true },
			{ .index = 2, .switch_number = 1, .button_number = 2, .assignable = true },
			{ .index = 3, .switch_number = 0, .button_number = 0, .assignable = false },
		};
		return remote;
	}

	struct SpasideFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		SpasideFixture()
			: controller(std::make_shared<StubSpasideController>())
		{
		}

		// The controller is only present when the Jandy stack runs; tests that want it call this
		// BEFORE sending (the route resolves it in its constructor).
		void RegisterController()
		{
			Register(std::static_pointer_cast<Interfaces::ISpasideRemoteController>(controller));
		}

		std::shared_ptr<Preferences::PreferencesService> MakePreferencesService()
		{
			Options::Preferences::PreferencesSettings settings; // empty file -> in-memory only
			return std::make_shared<Preferences::PreferencesService>(*this, settings);
		}

		HTTP::Response Send(boost::beast::http::verb verb, const std::string& body, std::shared_ptr<Preferences::PreferencesService> service = nullptr)
		{
			HTTP::WebRoute_Equipment_SpasideRemotes route(*this, std::move(service));

			HTTP::Request req;
			req.version(11);
			req.method(verb);
			req.target(HTTP::EQUIPMENT_SPASIDE_REMOTES_ROUTE_URL);
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

		HTTP::Response Get() { return Send(boost::beast::http::verb::get, ""); }
		HTTP::Response Post(const std::string& body, std::shared_ptr<Preferences::PreferencesService> service = nullptr) { return Send(boost::beast::http::verb::post, body, std::move(service)); }

		std::shared_ptr<Kernel::DataHub> DataHub() { return Find<Kernel::DataHub>(); }
		std::shared_ptr<Kernel::PreferencesHub> PreferencesHub() { return Find<Kernel::PreferencesHub>(); }

		std::shared_ptr<StubSpasideController> controller;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_HttpRoutes_SpasideRemotes, SpasideFixture)

//-----------------------------------------------------------------------------
// GET
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Get_NoController_ReturnsEmptyWellFormedEnvelope)
{
	auto resp = Get();
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(body["remotes"].is_array());
	BOOST_CHECK(body["remotes"].empty());
	BOOST_CHECK(body["assignments"].is_array());
	BOOST_CHECK(body["requested"].is_array());
	BOOST_CHECK(body["available_functions"].is_array());
	BOOST_CHECK(body["available_functions"].empty());
}

BOOST_AUTO_TEST_CASE(Get_WithRemote_JoinsLiveAndRequestedAssignmentsPerButton)
{
	RegisterController();
	controller->remotes.push_back(MakeDualSpaSwitch());
	controller->available_functions = { "Spa", "Spa Heat" };

	// Live decoded map: switch 1 button 1 -> "Pool Light" (NOT in the controller's picker list,
	// so it must be unioned into available_functions).
	DataHub()->SetSpaSwitchAssignment(1, 1, "Pool Light");

	// Persisted requests: one valid, and several malformed keys that must be skipped.
	auto& requested = PreferencesHub()->SpaSwitchButtons;
	requested["1:2"] = "Spa Light";
	requested["nocolon"] = "x";
	requested["1:x"] = "y";
	requested["300:1"] = "z";
	requested["1:300"] = "z";
	requested["2:2"] = 5; // non-string function

	auto resp = Get();
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	const auto body = nlohmann::json::parse(resp.body());

	BOOST_REQUIRE_EQUAL(body["remotes"].size(), 1u);
	const auto& remote = body["remotes"][0];
	BOOST_CHECK_EQUAL(remote["address"].get<std::string>(), "0x10");
	BOOST_CHECK_EQUAL(remote["device_class"].get<std::string>(), "DualSpaSwitch");
	BOOST_CHECK(remote["emulated"].get<bool>());
	BOOST_CHECK_EQUAL(remote["button_count"].get<int>(), 3);
	BOOST_CHECK_EQUAL(remote["poll_count"].get<int>(), 42);
	BOOST_CHECK_EQUAL(remote["last_button"].get<int>(), 2);
	BOOST_CHECK(remote["led_image_seen"].get<bool>());
	BOOST_REQUIRE_EQUAL(remote["leds"].size(), 3u);
	BOOST_CHECK_EQUAL(remote["leds"][2].get<std::string>(), "blink");
	BOOST_CHECK_EQUAL(remote["led_image"].get<std::string>(), "a5ff");

	BOOST_REQUIRE_EQUAL(remote["buttons"].size(), 3u);

	// Button 1: live function, no request -> not pending.
	const auto& b1 = remote["buttons"][0];
	BOOST_CHECK_EQUAL(b1["index"].get<int>(), 1);
	BOOST_CHECK_EQUAL(b1["switch"].get<int>(), 1);
	BOOST_CHECK_EQUAL(b1["button"].get<int>(), 1);
	BOOST_CHECK(b1["assignable"].get<bool>());
	BOOST_CHECK(b1["pressable"].get<bool>());
	BOOST_CHECK_EQUAL(b1["function"].get<std::string>(), "Pool Light");
	BOOST_CHECK_EQUAL(b1["requested"].get<std::string>(), "");
	BOOST_CHECK(!b1["pending"].get<bool>());

	// Button 2: request present, live map silent -> pending.
	const auto& b2 = remote["buttons"][1];
	BOOST_CHECK_EQUAL(b2["function"].get<std::string>(), "");
	BOOST_CHECK_EQUAL(b2["requested"].get<std::string>(), "Spa Light");
	BOOST_CHECK(b2["pending"].get<bool>());

	// Button 3: unmapped key -> never joined to either map.
	const auto& b3 = remote["buttons"][2];
	BOOST_CHECK(!b3["assignable"].get<bool>());
	BOOST_CHECK_EQUAL(b3["function"].get<std::string>(), "");
	BOOST_CHECK_EQUAL(b3["requested"].get<std::string>(), "");
	BOOST_CHECK(!b3["pending"].get<bool>());

	// Flat back-compat lists.
	BOOST_REQUIRE_EQUAL(body["assignments"].size(), 1u);
	BOOST_CHECK_EQUAL(body["assignments"][0]["switch"].get<int>(), 1);
	BOOST_CHECK_EQUAL(body["assignments"][0]["button"].get<int>(), 1);
	BOOST_CHECK_EQUAL(body["assignments"][0]["function"].get<std::string>(), "Pool Light");

	// Only the one well-formed request survives the parse.
	BOOST_REQUIRE_EQUAL(body["requested"].size(), 1u);
	BOOST_CHECK_EQUAL(body["requested"][0]["switch"].get<int>(), 1);
	BOOST_CHECK_EQUAL(body["requested"][0]["button"].get<int>(), 2);
	BOOST_CHECK_EQUAL(body["requested"][0]["function"].get<std::string>(), "Spa Light");

	// Controller order first, then the in-use function that the picker did not list.
	BOOST_REQUIRE_EQUAL(body["available_functions"].size(), 3u);
	BOOST_CHECK_EQUAL(body["available_functions"][0].get<std::string>(), "Spa");
	BOOST_CHECK_EQUAL(body["available_functions"][1].get<std::string>(), "Spa Heat");
	BOOST_CHECK_EQUAL(body["available_functions"][2].get<std::string>(), "Pool Light");
}

BOOST_AUTO_TEST_CASE(Get_RequestConfirmedByLiveMap_IsNotPending)
{
	RegisterController();
	controller->remotes.push_back(MakeDualSpaSwitch());
	controller->available_functions = { "Spa Light" };

	DataHub()->SetSpaSwitchAssignment(1, 2, "Spa Light");
	PreferencesHub()->SpaSwitchButtons["1:2"] = "Spa Light";

	const auto body = nlohmann::json::parse(Get().body());
	const auto& b2 = body["remotes"][0]["buttons"][1];
	BOOST_CHECK_EQUAL(b2["function"].get<std::string>(), "Spa Light");
	BOOST_CHECK_EQUAL(b2["requested"].get<std::string>(), "Spa Light");
	BOOST_CHECK(!b2["pending"].get<bool>());

	// Already in the picker list -> not duplicated.
	BOOST_REQUIRE_EQUAL(body["available_functions"].size(), 1u);
}

BOOST_AUTO_TEST_CASE(Get_RequestDiffersFromLiveMap_IsPending)
{
	RegisterController();
	controller->remotes.push_back(MakeDualSpaSwitch());

	DataHub()->SetSpaSwitchAssignment(1, 2, "Spa");
	PreferencesHub()->SpaSwitchButtons["1:2"] = "Spa Light";

	const auto body = nlohmann::json::parse(Get().body());
	const auto& b2 = body["remotes"][0]["buttons"][1];
	BOOST_CHECK_EQUAL(b2["function"].get<std::string>(), "Spa");
	BOOST_CHECK_EQUAL(b2["requested"].get<std::string>(), "Spa Light");
	BOOST_CHECK(b2["pending"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Get_ObservedOnlyRemote_IsNotPressable)
{
	RegisterController();
	auto remote = MakeDualSpaSwitch();
	remote.emulated = false;
	controller->remotes.push_back(remote);

	const auto body = nlohmann::json::parse(Get().body());
	BOOST_CHECK(!body["remotes"][0]["emulated"].get<bool>());
	BOOST_CHECK(!body["remotes"][0]["buttons"][0]["pressable"].get<bool>());
}

//-----------------------------------------------------------------------------
// Method handling
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Put_Returns405WithAllowedMethods)
{
	auto resp = Send(boost::beast::http::verb::put, "{}");
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, resp.result());

	const auto body = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(body["code"].get<std::string>(), "method_not_allowed");
	BOOST_CHECK_EQUAL(body["params"]["allowed"].get<std::string>(), "GET, POST");
}

//-----------------------------------------------------------------------------
// POST -- envelope-level validation
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Post_NoController_Returns503)
{
	auto resp = Post(R"({"action":"press","address":16,"button":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_unavailable");
}

BOOST_AUTO_TEST_CASE(Post_InvalidJson_Returns400)
{
	RegisterController();
	auto resp = Post("{ not json");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "invalid_json");
}

BOOST_AUTO_TEST_CASE(Post_MissingOrNonStringAction_Returns400)
{
	RegisterController();

	for (const auto* payload : { R"({})", R"({"action": 7})", R"({"address":16,"button":1})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_action_required");
	}
	BOOST_CHECK(controller->presses.empty());
	BOOST_CHECK(controller->assigns.empty());
}

BOOST_AUTO_TEST_CASE(Post_UnknownAction_Returns400)
{
	RegisterController();
	auto resp = Post(R"({"action":"hold","address":16,"button":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_invalid_action");
	BOOST_CHECK(controller->presses.empty());
}

//-----------------------------------------------------------------------------
// POST action=press
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Press_MissingOrInvalidAddress_Returns400)
{
	RegisterController();

	for (const auto* payload : { R"({"action":"press","button":1})", R"({"action":"press","address":-1,"button":1})", R"({"action":"press","address":"0x10","button":1})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_press_requires_address");
	}
	BOOST_CHECK(controller->presses.empty());
}

BOOST_AUTO_TEST_CASE(Press_MissingOrInvalidButton_Returns400)
{
	RegisterController();

	for (const auto* payload : { R"({"action":"press","address":16})", R"({"action":"press","address":16,"button":-2})", R"({"action":"press","address":16,"button":"1"})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_press_requires_button");
	}
	BOOST_CHECK(controller->presses.empty());
}

BOOST_AUTO_TEST_CASE(Press_ValueWiderThanAByte_Returns400)
{
	RegisterController();

	for (const auto* payload : { R"({"action":"press","address":256,"button":1})", R"({"action":"press","address":16,"button":256})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_byte_range");
	}
	BOOST_CHECK(controller->presses.empty());
}

BOOST_AUTO_TEST_CASE(Press_Success_QueuesPressAndReturnsEnvelope)
{
	RegisterController();
	controller->remotes.push_back(MakeDualSpaSwitch());

	auto resp = Post(R"({"action":"press","address":16,"button":2})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	BOOST_REQUIRE_EQUAL(controller->presses.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(controller->presses.front().first), 0x10);
	BOOST_CHECK_EQUAL(static_cast<int>(controller->presses.front().second), 2);

	// The success response carries the same envelope shape as GET.
	const auto body = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE_EQUAL(body["remotes"].size(), 1u);
	BOOST_CHECK_EQUAL(body["remotes"][0]["address"].get<std::string>(), "0x10");
	BOOST_CHECK(body.contains("available_functions"));
}

BOOST_AUTO_TEST_CASE(Press_RemoteNotFound_Returns404)
{
	RegisterController();
	controller->press_result = Interfaces::ISpasideRemoteController::PressResult::RemoteNotFound;

	auto resp = Post(R"({"action":"press","address":32,"button":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_remote_not_found");
	BOOST_REQUIRE_EQUAL(controller->presses.size(), 1u);
}

BOOST_AUTO_TEST_CASE(Press_NotEmulated_Returns409)
{
	RegisterController();
	controller->press_result = Interfaces::ISpasideRemoteController::PressResult::NotEmulated;

	auto resp = Post(R"({"action":"press","address":16,"button":1})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_remote_observed_only");
}

BOOST_AUTO_TEST_CASE(Press_InvalidButton_Returns400)
{
	RegisterController();
	controller->press_result = Interfaces::ISpasideRemoteController::PressResult::InvalidButton;

	auto resp = Post(R"({"action":"press","address":16,"button":9})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_button_out_of_range");
}

//-----------------------------------------------------------------------------
// POST action=assign
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Assign_MissingOrMistypedFields_Returns400)
{
	RegisterController();

	for (const auto* payload : {
		R"({"action":"assign","button":1,"function":"Spa"})",
		R"({"action":"assign","switch":1,"function":"Spa"})",
		R"({"action":"assign","switch":1,"button":1})",
		R"({"action":"assign","switch":-1,"button":1,"function":"Spa"})",
		R"({"action":"assign","switch":1,"button":"1","function":"Spa"})",
		R"({"action":"assign","switch":1,"button":1,"function":3})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_assign_requires_fields");
	}
	BOOST_CHECK(controller->assigns.empty());
}

BOOST_AUTO_TEST_CASE(Assign_ValueWiderThanAByte_Returns400)
{
	RegisterController();

	for (const auto* payload : { R"({"action":"assign","switch":256,"button":1,"function":"Spa"})", R"({"action":"assign","switch":1,"button":300,"function":"Spa"})" })
	{
		auto resp = Post(payload);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
		BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_byte_range");
	}
	BOOST_CHECK(controller->assigns.empty());
}

BOOST_AUTO_TEST_CASE(Assign_Accepted_WithPreferencesService_RecordsRequestAndReturnsEnvelope)
{
	RegisterController();
	controller->remotes.push_back(MakeDualSpaSwitch());
	controller->available_functions = { "Spa Light" };

	auto resp = Post(R"({"action":"assign","switch":1,"button":2,"function":"Spa Light"})", MakePreferencesService());
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	BOOST_REQUIRE_EQUAL(controller->assigns.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(std::get<0>(controller->assigns.front())), 1);
	BOOST_CHECK_EQUAL(static_cast<int>(std::get<1>(controller->assigns.front())), 2);
	BOOST_CHECK_EQUAL(std::get<2>(controller->assigns.front()), "Spa Light");

	// The desired-state record lands in the PreferencesHub ...
	BOOST_REQUIRE(PreferencesHub()->SpaSwitchButtons.contains("1:2"));
	BOOST_CHECK_EQUAL(PreferencesHub()->SpaSwitchButtons["1:2"].get<std::string>(), "Spa Light");

	// ... and the returned envelope already reflects it as a pending request on that button.
	const auto body = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE_EQUAL(body["requested"].size(), 1u);
	BOOST_CHECK_EQUAL(body["requested"][0]["function"].get<std::string>(), "Spa Light");
	const auto& b2 = body["remotes"][0]["buttons"][1];
	BOOST_CHECK_EQUAL(b2["requested"].get<std::string>(), "Spa Light");
	BOOST_CHECK(b2["pending"].get<bool>());
}

BOOST_AUTO_TEST_CASE(Assign_Accepted_WithoutPreferencesService_StillProgramsButDoesNotPersist)
{
	RegisterController();

	auto resp = Post(R"({"action":"assign","switch":1,"button":2,"function":"Spa Light"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(controller->assigns.size(), 1u);
	BOOST_CHECK(!PreferencesHub()->SpaSwitchButtons.contains("1:2"));
	BOOST_CHECK(nlohmann::json::parse(resp.body())["requested"].empty());
}

BOOST_AUTO_TEST_CASE(Assign_InvalidRequest_Returns400)
{
	RegisterController();
	controller->assign_result = Interfaces::ISpasideRemoteController::AssignResult::InvalidRequest;

	auto resp = Post(R"({"action":"assign","switch":1,"button":2,"function":"Nonsense"})", MakePreferencesService());
	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_assign_invalid");
	// A rejected request must NOT be remembered as desired state.
	BOOST_CHECK(!PreferencesHub()->SpaSwitchButtons.contains("1:2"));
}

BOOST_AUTO_TEST_CASE(Assign_Busy_Returns409)
{
	RegisterController();
	controller->assign_result = Interfaces::ISpasideRemoteController::AssignResult::Busy;

	auto resp = Post(R"({"action":"assign","switch":1,"button":2,"function":"Spa"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_controller_busy");
}

BOOST_AUTO_TEST_CASE(Assign_NotAvailable_Returns503)
{
	RegisterController();
	controller->assign_result = Interfaces::ISpasideRemoteController::AssignResult::NotAvailable;

	auto resp = Post(R"({"action":"assign","switch":1,"button":2,"function":"Spa"})");
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(resp.body())["code"].get<std::string>(), "spaside_no_programmer");
}

BOOST_AUTO_TEST_SUITE_END()
