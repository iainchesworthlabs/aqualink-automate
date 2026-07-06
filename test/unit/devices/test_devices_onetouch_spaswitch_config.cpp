#include <algorithm>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/onetouch_device.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_onetouchdevice.h"

using namespace AqualinkAutomate;

//=============================================================================
// OneTouch spa-switch config (Phase 4b, open-source generality). Two halves:
//
//   READ:  the "Spa Switch" menu page lists button assignments as
//          "<switch>:<button>   <function>" screen lines. PageProcessor_SpaSwitch
//          decodes them into the controller-agnostic DataHub map -- the OneTouch
//          half of the same read path the iAQ provides.
//
//   WRITE: SpaSwitchConfigurator::SetSpaSwitchAssignment programs a button's
//          function over the bus by driving the emulated keypad through the Spa
//          Switch config menu (System Setup -> Spa Switch -> the number page ->
//          Button Setup -> the "S:B" row -> the function picker). These tests
//          cover the capability contract (request gating) and the picker-compare
//          primitive (SanitiseFunctionText). The end-to-end keypress SEQUENCE is
//          verified against captures/spaside_setup_nav.cap by hand -- consistent
//          with the OneTouch setpoint/chlorinator flow tests, which likewise
//          hand-verify the in-menu value-stepping rather than re-decode it.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(OneTouchSpaSwitchConfig_TestSuite, Test::OneTouchDevice)

BOOST_AUTO_TEST_CASE(SpaSwitchPage_PopulatesAssignmentMap)
{
	InitialiseOneTouchDevice();

	// The real OneTouch "Spa Switch" page (title at line 0, assignments on the rows below).
	LoadAndSignalTestPage({
		{ 0, "   Spa Switch" },
		{ 3, "1:1     Spa Jets" },
		{ 4, "1:2   Pool Light" },
		{ 5, "1:3   Air Blower" },
		{ 6, "1:4     Spillway" },
		{ 7, "2:1     Swim Jet" },
		{ 8, "2:4   Pool Light" },
	});

	auto& hub = DataHub();
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(1, 1).value_or(""), "Spa Jets");
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(1, 2).value_or(""), "Pool Light");
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(1, 3).value_or(""), "Air Blower");
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(1, 4).value_or(""), "Spillway");
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(2, 1).value_or(""), "Swim Jet");
	BOOST_CHECK_EQUAL(hub.SpaSwitchAssignment(2, 4).value_or(""), "Pool Light");
}

BOOST_AUTO_TEST_CASE(NonSpaSwitchPage_DoesNotMisparseClock)
{
	// The home screen shows the time as "1:20 PM" -- which looks like "switch 1, button 20" to a
	// naive parser. Page gating (the "Spa Switch" detector) means a non-config page is never fed to
	// the assignment parser, so the clock must NOT create a bogus assignment.
	InitialiseOneTouchDevice();

	LoadAndSignalTestPage({
		{ 0, "Jandy AquaLinkRS" },
		{ 3, "    1:20 PM" },
		{ 5, "   Pool 22`C" },
	});

	BOOST_CHECK(DataHub().SpaSwitchAssignments().empty());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// WRITE path: SanitiseFunctionText -- the picker-compare primitive. The OneTouch
// row Text is already the clean displayed content (the inverse-video cursor is a
// separate Highlight message, never folded into the row Text), so the compare just
// trims surrounding whitespace / non-printable bytes. The executor uses this to
// match the picker's selected-function line against the requested function.
//=============================================================================

BOOST_AUTO_TEST_SUITE(OneTouchSpaSwitchSanitise_TestSuite)

BOOST_AUTO_TEST_CASE(SanitiseFunctionText_TrimsEdgesPreservesInteriorSpaces)
{
	using Devices::OneTouchDevice;

	// Surrounding spaces trimmed; the function name itself (incl. interior spaces) preserved.
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText("Pool Light"), "Pool Light");
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText("   Pool Light   "), "Pool Light");
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText("Air Blower"), "Air Blower");

	// Whitespace control bytes (tab/CR/LF) at the edges are also trimmed.
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText("\tSpa Jets\r\n"), "Spa Jets");

	// Non-printable artefacts at the edges (e.g. a stray cursor/highlight byte that leaked
	// into the raw row) are stripped, leaving the displayed text.
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText(std::string("\x01Spillway\x7f", 10)), "Spillway");
}

BOOST_AUTO_TEST_CASE(SanitiseFunctionText_BlankOrEmptyRowYieldsEmpty)
{
	using Devices::OneTouchDevice;

	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText(""), "");
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText("                "), "");
	BOOST_CHECK_EQUAL(Devices::OneTouch::SanitiseFunctionText(std::string("\0\0\0", 3)), "");
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// WRITE path: SetSpaSwitchAssignment request gating (the SpaSwitchConfigurator
// capability contract). Programming a switch button drives the EMULATED keypad
// through the config menu, so a passive (real-observed) OneTouch cannot do it; the
// request is validated up-front and only one goal runs at a time on the shared keypad.
//=============================================================================

namespace
{
	struct OneTouchAssignFixture : public Test::HubLocatorInjector
	{
		std::shared_ptr<Devices::OneTouchDevice> MakeDevice(bool emulated)
		{
			auto id = std::make_shared<Devices::JandyDeviceType>(Devices::JandyDeviceId(0x40));
			return std::make_shared<Devices::OneTouchDevice>(id, *this, emulated);
		}
	};
}

BOOST_FIXTURE_TEST_SUITE(OneTouchSpaSwitchAssign_TestSuite, OneTouchAssignFixture)

BOOST_AUTO_TEST_CASE(SetSpaSwitchAssignment_Emulated_ValidRequest_IsAccepted)
{
	auto dev = MakeDevice(/*emulated*/ true);
	BOOST_CHECK(dev->SetSpaSwitchAssignment(1, 2, "Pool Light") == Devices::Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(SetSpaSwitchAssignment_NotEmulated_IsNotSupported)
{
	// A passively-observed OneTouch never transmits keys, so it cannot program anything.
	auto dev = MakeDevice(/*emulated*/ false);
	BOOST_CHECK(dev->SetSpaSwitchAssignment(1, 2, "Pool Light") == Devices::Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(SetSpaSwitchAssignment_InvalidArgs_IsInvalidValue)
{
	auto dev = MakeDevice(/*emulated*/ true);
	BOOST_CHECK(dev->SetSpaSwitchAssignment(0, 1, "Pool Light") == Devices::Capabilities::ActuationResult::InvalidValue);   // switch < 1
	BOOST_CHECK(dev->SetSpaSwitchAssignment(1, 0, "Pool Light") == Devices::Capabilities::ActuationResult::InvalidValue);   // button < 1
	BOOST_CHECK(dev->SetSpaSwitchAssignment(1, 1, "")           == Devices::Capabilities::ActuationResult::InvalidValue);   // empty function
}

BOOST_AUTO_TEST_CASE(SetSpaSwitchAssignment_OneAtATime_SecondRequestRejectedWhileBusy)
{
	auto dev = MakeDevice(/*emulated*/ true);
	// First request queues a goal on the single shared keypad...
	BOOST_REQUIRE(dev->SetSpaSwitchAssignment(1, 1, "Spa Jets") == Devices::Capabilities::ActuationResult::Accepted);
	// ...so a second must not interleave until the first completes.
	BOOST_CHECK(dev->SetSpaSwitchAssignment(2, 3, "Pool Light") == Devices::Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(ControllerPriority_IsLow_MenuNavigationChannel)
{
	// The OneTouch programs via menu navigation, so it ranks below a direct-channel
	// controller (e.g. iAQ page-edit) when both can configure spa-switch buttons.
	auto dev = MakeDevice(/*emulated*/ true);
	BOOST_CHECK(dev->ControllerPriority() == Devices::Capabilities::ActuationPriority::Low);
}

BOOST_AUTO_TEST_CASE(AvailableFunctions_AreTheControllerPickerSet)
{
	// The assignable-function list (the bound the UI offers) is the controller's picker set.
	auto dev = MakeDevice(/*emulated*/ true);
	const auto functions = dev->AvailableFunctions();
	BOOST_REQUIRE(!functions.empty());
	BOOST_CHECK(std::find(functions.begin(), functions.end(), "Pool Light") != functions.end());
	BOOST_CHECK(std::find(functions.begin(), functions.end(), "All OFF") != functions.end());
}

BOOST_AUTO_TEST_SUITE_END()
