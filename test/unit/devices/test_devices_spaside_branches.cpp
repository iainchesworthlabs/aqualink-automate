#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/spaside_remote_controller.h"
#include "jandy/devices/spaside_remote_device.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_probe.h"
#include "jandy/utility/jandy_null_handler.h"
#include "jandy/messages/jandy_message_status.h"

#include "kernel/equipment_hub.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// Spa-side remotes -- the EMULATED transmit path (we ARE the keypad: answer the
// master's probe and LED-image poll, injecting a momentary button press), the
// diagnostics projection of the decoded LED image, and the control surface's
// no-hub / non-remote / configurator-enumeration arms.
//
// The existing suites cover the PASSIVE decode of a real remote and the happy
// paths of the control surface; these are the arms they do not reach.
//=============================================================================

namespace
{
	constexpr uint8_t DUAL_SPA_SWITCH_ADDRESS{ 0x10 };   // "2x4rem" Dual Spa Switch, 8 keys
	constexpr uint8_t SPA_LINK_ADDRESS{ 0x20 };          // "8button" Spa Link, 9 keys
	constexpr uint8_t ONETOUCH_ADDRESS{ 0x40 };          // NOT a spa-side class
	constexpr uint8_t IAQ_ADDRESS{ 0x33 };

	// A master -> remote cmd-0x02 LED-image poll. With a 5-byte payload the framed message is
	// exactly JandyMessage_Status's minimum, so it decodes as a Status addressed to `dest`.
	void EmitLedPollTo(uint8_t dest, uint8_t led_byte)
	{
		const auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(dest, 0x02, { led_byte, 0x11, 0x22, 0x00, 0x00 });
		Messages::JandyMessage_Status msg;
		const auto span = std::as_bytes(std::span<const uint8_t>(frame.data(), frame.size()));
		BOOST_REQUIRE(msg.Deserialize(span));
		(*Messages::JandyMessage_Status::GetSignal())(msg);
	}

	// A master -> remote discovery probe (cmd 0x00).
	//
	// The frame has to be DLE-null escaped before it is handed to Deserialize(). A probe
	// carries command 0x00, so for a destination of 0x10 (the Dual Spa Switch) the raw
	// frame contains the byte pair 10 00 -- exactly the escape sequence the wire uses for
	// a literal 0x10 in the data. Unescaped, the deserialiser collapses that pair back to
	// a single 0x10 and the frame no longer parses. Escaping on the way in (the same thing
	// the real serialiser does before transmitting) round-trips correctly for every
	// destination, colliding or not.
	void EmitProbeTo(uint8_t dest)
	{
		auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(dest, 0x00, {});
		Utility::JandyPacket_NullCharHandler_Serialization(frame);

		Messages::JandyMessage_Probe msg;
		const auto span = std::as_bytes(std::span<const uint8_t>(frame.data(), frame.size()));
		BOOST_REQUIRE(msg.Deserialize(span));
		(*Messages::JandyMessage_Probe::GetSignal())(msg);
	}

	// Captures every Ack the device under test puts on the wire (the send publisher, not the
	// received-message signal), so a test can assert the emulated button report.
	class AckRecorder
	{
	public:
		AckRecorder()
		{
			m_Connection = Messages::JandyMessage_Ack::GetPublisher()->connect(
				[this](std::reference_wrapper<const Messages::JandyMessage_Ack> ack)
				{
					commands.push_back(ack.get().Command());
				});
		}

		std::vector<uint8_t> commands;

	private:
		boost::signals2::scoped_connection m_Connection;
	};

	struct SpasideDeviceFixture : public Test::HubLocatorInjector
	{
		std::shared_ptr<JandyDeviceType> Id(uint8_t address) const
		{
			return std::make_shared<JandyDeviceType>(JandyDeviceId(address));
		}
	};

	struct SpasideControllerFixture : public Test::HubLocatorInjector
	{
		SpasideControllerFixture() : hub(std::make_shared<Kernel::EquipmentHub>()) {}

		void AddRemote(uint8_t address, bool emulated)
		{
			auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(address));
			hub->AddDevice(std::make_unique<SpasideRemoteDevice>(id, *this, emulated));
		}

		void AddIAQ()
		{
			auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_ADDRESS));
			hub->AddDevice(std::make_unique<IAQDevice>(id, *this, /*emulated=*/true));
		}

		void AddOneTouch()
		{
			auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(ONETOUCH_ADDRESS));
			hub->AddDevice(std::make_unique<OneTouchDevice>(id, *this, /*emulated=*/true));
		}

		std::shared_ptr<Kernel::EquipmentHub> hub;
	};
}
// unnamed namespace

//=============================================================================
BOOST_FIXTURE_TEST_SUITE(SpasideRemoteDeviceBranches_TestSuite, SpasideDeviceFixture)
//=============================================================================

BOOST_AUTO_TEST_CASE(Emulated_AnswersTheDiscoveryProbeWithAnIdleButtonReport)
{
	// We ARE the keypad: the master's discovery probe must be answered, or the master never
	// polls us. With nothing pressed the reply carries button 0 (idle).
	SpasideRemoteDevice device(Id(DUAL_SPA_SWITCH_ADDRESS), *this, /*is_emulated=*/true);
	AckRecorder acks;

	EmitProbeTo(DUAL_SPA_SWITCH_ADDRESS);

	BOOST_REQUIRE_EQUAL(acks.commands.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(acks.commands[0]), 0);
	BOOST_CHECK_EQUAL(device.PollCount(), 1u);
}

BOOST_AUTO_TEST_CASE(Emulated_ProbeToAnotherAddress_IsNotAnswered)
{
	SpasideRemoteDevice device(Id(DUAL_SPA_SWITCH_ADDRESS), *this, /*is_emulated=*/true);
	AckRecorder acks;

	EmitProbeTo(SPA_LINK_ADDRESS);

	BOOST_CHECK(acks.commands.empty());
	BOOST_CHECK_EQUAL(device.PollCount(), 0u);
}

BOOST_AUTO_TEST_CASE(Emulated_AQueuedPressIsReportedOnceThenReleased)
{
	// A physical keypress is MOMENTARY: it must be reported to the master exactly once, then
	// released. Repeating it would look like a held key and re-actuate the equipment.
	SpasideRemoteDevice device(Id(DUAL_SPA_SWITCH_ADDRESS), *this, /*is_emulated=*/true);
	AckRecorder acks;

	device.PressButton(5);
	EmitLedPollTo(DUAL_SPA_SWITCH_ADDRESS, 0x00);
	EmitLedPollTo(DUAL_SPA_SWITCH_ADDRESS, 0x00);

	BOOST_REQUIRE_EQUAL(acks.commands.size(), 2u);
	BOOST_CHECK_EQUAL(static_cast<int>(acks.commands[0]), 5);   // reported...
	BOOST_CHECK_EQUAL(static_cast<int>(acks.commands[1]), 0);   // ...then released
	BOOST_CHECK_EQUAL(device.PollCount(), 2u);
}

BOOST_AUTO_TEST_CASE(Emulated_AlsoDisplaysTheLedImageTheMasterPushes)
{
	// An emulated remote still "shows" whatever the master pushes, so the same indicator decode
	// runs on the emulated path as on the passive one.
	SpasideRemoteDevice device(Id(SPA_LINK_ADDRESS), *this, /*is_emulated=*/true);
	AckRecorder acks;

	BOOST_CHECK(!device.LedImageSeen());
	BOOST_CHECK(device.LedStates().empty());     // nothing decoded yet
	BOOST_CHECK(device.LedImageHex().empty());

	EmitLedPollTo(SPA_LINK_ADDRESS, 0x39);       // 0b00'11'10'01 -> on, blink, blink, off

	BOOST_REQUIRE(device.LedImageSeen());
	const auto states = device.LedStates();
	BOOST_REQUIRE_EQUAL(states.size(), 4u);
	BOOST_CHECK_EQUAL(states[0], "on");
	BOOST_CHECK_EQUAL(states[1], "blink");
	BOOST_CHECK_EQUAL(states[2], "blink");
	BOOST_CHECK_EQUAL(states[3], "off");

	// The raw image is retained verbatim for the still-undecoded higher bytes.
	BOOST_CHECK_EQUAL(device.LedImageHex(), std::string("39 11 22 00 00"));
}

BOOST_AUTO_TEST_CASE(Diagnostics_BeforeAnyTraffic_ReportNoLedImageAndNoButtonAge)
{
	SpasideRemoteDevice device(Id(DUAL_SPA_SWITCH_ADDRESS), *this, /*is_emulated=*/false);

	const auto diagnostics = device.DescribeDiagnostics();
	BOOST_CHECK_EQUAL(diagnostics["device_type"].get<std::string>(), std::string("SpasideRemote"));
	BOOST_CHECK_EQUAL(diagnostics["device_id"].get<std::string>(), std::string("0x10"));
	BOOST_CHECK_EQUAL(diagnostics["poll_count"].get<int64_t>(), 0);
	BOOST_CHECK_EQUAL(diagnostics["last_button"].get<int>(), 0);
	BOOST_CHECK_EQUAL(diagnostics["led_image_seen"].get<bool>(), false);
	BOOST_CHECK(!diagnostics.contains("leds"));         // omitted until an image is decoded
	BOOST_CHECK(!diagnostics.contains("led_image"));
	BOOST_CHECK(diagnostics["last_button_age_seconds"].is_null());
	BOOST_CHECK_EQUAL(diagnostics["is_emulated"].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(Diagnostics_AfterAPollAndAPress_CarryTheDecodedState)
{
	SpasideRemoteDevice device(Id(DUAL_SPA_SWITCH_ADDRESS), *this, /*is_emulated=*/false);

	EmitLedPollTo(DUAL_SPA_SWITCH_ADDRESS, 0x01);   // LED0 on, the rest off

	// The remote's reply: an ack_type-0x00 report carrying button 3.
	{
		const auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(0x00, 0x01, { 0x00, 0x03 });
		Messages::JandyMessage_Ack ack;
		const auto span = std::as_bytes(std::span<const uint8_t>(frame.data(), frame.size()));
		BOOST_REQUIRE(ack.Deserialize(span));
		(*Messages::JandyMessage_Ack::GetSignal())(ack);
	}

	const auto diagnostics = device.DescribeDiagnostics();
	BOOST_CHECK_EQUAL(diagnostics["poll_count"].get<int64_t>(), 1);
	BOOST_CHECK_EQUAL(diagnostics["last_button"].get<int>(), 3);
	BOOST_CHECK_EQUAL(diagnostics["led_image_seen"].get<bool>(), true);
	BOOST_REQUIRE(diagnostics.contains("leds"));
	BOOST_CHECK_EQUAL(diagnostics["leds"][0].get<std::string>(), std::string("on"));
	BOOST_CHECK_EQUAL(diagnostics["leds"][1].get<std::string>(), std::string("off"));
	BOOST_CHECK_EQUAL(diagnostics["led_image"].get<std::string>(), std::string("01 11 22 00 00"));
	// A press was seen, so the age is a number (freshly recorded -> 0 seconds).
	BOOST_REQUIRE(diagnostics["last_button_age_seconds"].is_number());
	BOOST_CHECK_GE(diagnostics["last_button_age_seconds"].get<int64_t>(), 0);
	BOOST_CHECK_LT(diagnostics["last_button_age_seconds"].get<int64_t>(), 5);
}

BOOST_AUTO_TEST_CASE(AnUnrecognisedDeviceClass_HasNoButtons)
{
	// Only the two decoded spa-side classes have a known key count; anything else reports zero
	// keys and an empty layout rather than fabricating one.
	SpasideRemoteDevice device(Id(ONETOUCH_ADDRESS), *this, /*is_emulated=*/false);

	BOOST_CHECK_EQUAL(static_cast<int>(device.ButtonCount()), 0);
	BOOST_CHECK(device.ButtonLayout().empty());
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["device_id"].get<std::string>(), std::string("0x40"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_FIXTURE_TEST_SUITE(SpasideRemoteControllerBranches_TestSuite, SpasideControllerFixture)
//=============================================================================

BOOST_AUTO_TEST_CASE(WithoutAnEquipmentHub_EveryOperationDegradesSafely)
{
	// The control surface is registered before the hub exists in some start-up orders; it must
	// answer honestly rather than dereference a null hub.
	SpasideRemoteController controller(nullptr);
	using AR = Interfaces::ISpasideRemoteController::AssignResult;
	using PR = Interfaces::ISpasideRemoteController::PressResult;

	BOOST_CHECK(controller.Remotes().empty());
	BOOST_CHECK(controller.PressButton(DUAL_SPA_SWITCH_ADDRESS, 1) == PR::RemoteNotFound);
	BOOST_CHECK(controller.SetButtonAssignment(1, 1, "Pool Light") == AR::NotAvailable);
	BOOST_CHECK(controller.AvailableFunctions().empty());
}

BOOST_AUTO_TEST_CASE(Remotes_IgnoresDevicesThatAreNotSpasideRemotes)
{
	// The hub carries every device; only the spa-side keypads belong in this listing.
	AddIAQ();
	AddOneTouch();
	AddRemote(SPA_LINK_ADDRESS, /*emulated=*/true);

	SpasideRemoteController controller(hub);
	const auto remotes = controller.Remotes();

	BOOST_REQUIRE_EQUAL(remotes.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(remotes[0].address), static_cast<int>(SPA_LINK_ADDRESS));
	BOOST_CHECK_EQUAL(remotes[0].device_class, std::string("SpaRemote"));
	BOOST_CHECK_EQUAL(remotes[0].buttons.size(), 9u);
	// A Spa Link's key -> switch:button mapping is undecoded, so no key is programmable.
	for (const auto& button : remotes[0].buttons)
	{
		BOOST_CHECK(!button.assignable);
		BOOST_CHECK_EQUAL(static_cast<int>(button.switch_number), 0);
	}
}

BOOST_AUTO_TEST_CASE(Remotes_CarryTheDualSpaSwitchConfigCoordinates)
{
	// The 6588 board bridges Switch 2 (keys 1-4) and Switch 3 (keys 5-8); the control surface
	// copies that mapping out of the device rather than re-deriving it.
	AddRemote(DUAL_SPA_SWITCH_ADDRESS, /*emulated=*/false);

	SpasideRemoteController controller(hub);
	const auto remotes = controller.Remotes();
	BOOST_REQUIRE_EQUAL(remotes.size(), 1u);
	BOOST_REQUIRE_EQUAL(remotes[0].buttons.size(), 8u);

	BOOST_CHECK_EQUAL(static_cast<int>(remotes[0].buttons[0].switch_number), 2);
	BOOST_CHECK_EQUAL(static_cast<int>(remotes[0].buttons[0].button_number), 1);
	BOOST_CHECK_EQUAL(static_cast<int>(remotes[0].buttons[4].switch_number), 3);
	BOOST_CHECK_EQUAL(static_cast<int>(remotes[0].buttons[4].button_number), 1);
	BOOST_CHECK(remotes[0].buttons[7].assignable);
	// No LED image has been pushed yet, so the projection carries empty indicator state.
	BOOST_CHECK(!remotes[0].led_image_seen);
	BOOST_CHECK(remotes[0].leds.empty());
	BOOST_CHECK(remotes[0].led_image.empty());
}

BOOST_AUTO_TEST_CASE(SetButtonAssignment_AControllerRejectingTheValue_IsAnInvalidRequest)
{
	// Button 5 is inside the control surface's own generous bounds (1..16) but outside what the
	// iAQ's 4-function detail can express -- a rejected VALUE, not a missing capability, so it
	// must surface as InvalidRequest (400) rather than NotAvailable (503).
	AddIAQ();

	SpasideRemoteController controller(hub);
	BOOST_CHECK(controller.SetButtonAssignment(1, 5, "Pool Light")
		== Interfaces::ISpasideRemoteController::AssignResult::InvalidRequest);
}

BOOST_AUTO_TEST_CASE(AvailableFunctions_UnionTheConfigurators_WithoutDuplicates)
{
	// Both controllers report the same canonical set today, so the union must dedupe rather than
	// hand the UI every function twice.
	AddIAQ();
	AddOneTouch();

	SpasideRemoteController controller(hub);
	const auto functions = controller.AvailableFunctions();

	BOOST_REQUIRE(!functions.empty());
	for (std::size_t i = 0; i < functions.size(); ++i)
	{
		for (std::size_t j = i + 1; j < functions.size(); ++j)
		{
			BOOST_CHECK_NE(functions[i], functions[j]);
		}
	}
	BOOST_CHECK(std::find(functions.begin(), functions.end(), std::string("Pool Light")) != functions.end());
}

BOOST_AUTO_TEST_CASE(AvailableFunctions_WithNoConfigurator_IsEmpty)
{
	// A spa-side remote is not a configurator: with no controller present there is nothing to
	// offer, and the surface must not invent a list.
	AddRemote(DUAL_SPA_SWITCH_ADDRESS, /*emulated=*/true);

	SpasideRemoteController controller(hub);
	BOOST_CHECK(controller.AvailableFunctions().empty());
}

BOOST_AUTO_TEST_SUITE_END()
