#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "devices/capabilities/screen.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/jandy_message_display_update.h"
#include "jandy/messages/jandy_message_message_long.h"
#include "jandy/messages/jandy_message_probe.h"
#include "jandy/messages/jandy_message_status.h"
#include "jandy/messages/jandy_message_unknown.h"
#include "jandy/messages/pda/pda_message_clear.h"
#include "jandy/messages/pda/pda_message_highlight.h"
#include "jandy/messages/pda/pda_message_highlight_chars.h"
#include "jandy/messages/pda/pda_message_shiftlines.h"
#include "kernel/data_hub.h"
#include "utility/screen_data_page.h"
#include "utility/screen_data_page_processor.h"

#include "support/onetouch_test_device.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// OneTouchMessageRouter (devices/onetouch/onetouch_message_router.cpp): the slot
// handlers that turn each RS-485 frame addressed to our device id into a screen
// update, a controller tick and a watchdog kick.
//
// Every test drives the REAL signal/slot path - each message type is signalled the
// way the protocol read loop signals it - and asserts the two observable outcomes:
// the rendered screen (Screen capability) and whether the controller ticked. The
// tick is visible because a NON-emulated device leaves ColdStart for
// NormalOperation on its very first ProcessControllerUpdates, so a handler that
// deliberately does NOT tick (the ACK handler, and the out-of-range MessageLong
// rejection) leaves the device in ColdStart.
//
// The device id is 0x00 because a default-constructed message carries destination
// 0x00, which is what the per-device slot filter matches on.
//=============================================================================

namespace
{
	using TestDevice = Test::SeamedOneTouchDevice;
	using HighlightStates = Utility::ScreenDataPage::HighlightStates;

	// A full Jandy frame whose payload starts at wire index 4, which is where the PDA
	// messages read their line/index fields from.
	std::vector<uint8_t> MakeFrame(uint8_t message_type, const std::vector<uint8_t>& payload)
	{
		std::vector<uint8_t> frame{ 0x10, 0x02, 0x00, message_type };
		frame.insert(frame.end(), payload.begin(), payload.end());
		frame.push_back(0x00);
		frame.push_back(0x10);
		frame.push_back(0x03);
		return frame;
	}

	struct RouterFixture : public Test::HubLocatorInjector
	{
		RouterFixture() :
			device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x00))),
			device(device_type, *this, /*emulated*/ false)
		{
		}

		std::string OperatingState() const
		{
			return device.DescribeDiagnostics().at("operating_state").get<std::string>();
		}

		static void SendMessageLong(uint8_t line_id, const std::string& text)
		{
			Messages::JandyMessage_MessageLong msg(line_id, text);
			msg.Signal_MessageWasReceived();
		}

		static void SendStatus()
		{
			Messages::JandyMessage_Status msg;
			msg.Signal_MessageWasReceived();
		}

		static void SendHighlight(uint8_t line_id)
		{
			Messages::PDAMessage_Highlight msg(line_id);
			msg.Signal_MessageWasReceived();
		}

		static void SendHighlightChars(uint8_t line_id, uint8_t start_index, uint8_t stop_index)
		{
			Messages::PDAMessage_HighlightChars msg;
			const auto frame = MakeFrame(0x08, { line_id, start_index, stop_index });
			BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(frame)));
			msg.Signal_MessageWasReceived();
		}

		static void SendShiftLines(uint8_t first_line, uint8_t last_line, int8_t shift)
		{
			Messages::PDAMessage_ShiftLines msg;
			const auto frame = MakeFrame(0x09, { first_line, last_line, static_cast<uint8_t>(shift) });
			BOOST_REQUIRE(msg.DeserializeContents(std::span<const uint8_t>(frame)));
			msg.Signal_MessageWasReceived();
		}

		// Render four labelled rows and complete the update, so a shift has something to move.
		void RenderFourRows()
		{
			SendMessageLong(0, "Row0            ");
			SendMessageLong(1, "Row1            ");
			SendMessageLong(2, "Row2            ");
			SendMessageLong(3, "Row3            ");
			SendStatus();
		}

		std::shared_ptr<JandyDeviceType> device_type;
		TestDevice device;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_MessageRouter, RouterFixture)

//=============================================================================
// Slot_OneTouch_Ack
//=============================================================================

BOOST_AUTO_TEST_CASE(Ack_DecodesTheKeyPressButNeverTicksTheController)
{
	BOOST_REQUIRE_EQUAL(OperatingState(), "ColdStart");

	// A recognised key command (Select = 0x04) and an unrecognised one both reach the handler.
	Messages::JandyMessage_Ack known(Messages::AckTypes::V1_Normal, 0x04);
	known.Signal_MessageWasReceived();
	BOOST_CHECK_EQUAL(OperatingState(), "ColdStart");

	Messages::JandyMessage_Ack unknown(Messages::AckTypes::V1_Normal, 0x7F);
	unknown.Signal_MessageWasReceived();
	BOOST_CHECK_EQUAL(OperatingState(), "ColdStart");

	// The slot really is wired up for this device id: a message type that DOES tick moves the
	// (non-emulated) device on immediately.
	Messages::JandyMessage_Probe probe;
	probe.Signal_MessageWasReceived();
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

//=============================================================================
// Slot_OneTouch_MessageLong
//=============================================================================

BOOST_AUTO_TEST_CASE(MessageLong_RendersTheLineAndTicksTheController)
{
	SendMessageLong(4, "Filter Pump  ***");

	BOOST_CHECK_EQUAL(device.DisplayedPage()[4].Text, std::string{ "Filter Pump  ***" });
	BOOST_CHECK(device.ScreenMode() == Capabilities::ScreenModes::Updating);
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_CASE(MessageLong_LineBeyondTheScreen_IsRejectedWithoutTicking)
{
	// The OneTouch screen is 12 lines; anything at or past that is a malformed frame and is
	// dropped without touching the screen OR the controller state machine.
	SendMessageLong(12, "Off the bottom  ");
	SendMessageLong(200, "Way off         ");

	for (std::size_t line = 0; line < device.DisplayedPage().Size(); ++line)
	{
		BOOST_CHECK(device.DisplayedPage()[line].Text.empty());
	}
	BOOST_CHECK_EQUAL(OperatingState(), "ColdStart");
}

//=============================================================================
// Slot_OneTouch_Status
//=============================================================================

BOOST_AUTO_TEST_CASE(Status_CompletesTheScreenUpdateAndRunsThePageProcessors)
{
	SendMessageLong(9, "Equipment ON/OFF");
	BOOST_REQUIRE(device.ScreenMode() == Capabilities::ScreenModes::Updating);

	SendStatus();

	// The burst is finished: the page was processed (recognised as the home page) and the
	// screen is back to Normal awaiting the next burst.
	BOOST_CHECK(device.DisplayedPageType() == Utility::ScreenDataPageTypes::Page_System);
	BOOST_CHECK(device.ScreenMode() == Capabilities::ScreenModes::Normal);
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_CASE(Status_WithNoScreenUpdateInFlight_StillTicksTheController)
{
	// No MessageLong burst preceded this Status, so the screen mode is left alone.
	SendStatus();

	BOOST_CHECK(device.ScreenMode() == Capabilities::ScreenModes::Normal);
	BOOST_CHECK(device.DisplayedPageType() == Utility::ScreenDataPageTypes::Page_Unknown);
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

//=============================================================================
// Slot_OneTouch_Clear
//=============================================================================

BOOST_AUTO_TEST_CASE(Clear_BlanksTheWholeScreenAndTicksTheController)
{
	RenderFourRows();
	BOOST_REQUIRE_EQUAL(device.DisplayedPage()[0].Text, std::string{ "Row0            " });

	Messages::PDAMessage_Clear clear;
	clear.Signal_MessageWasReceived();

	for (std::size_t line = 0; line < device.DisplayedPage().Size(); ++line)
	{
		BOOST_CHECK(device.DisplayedPage()[line].Text.empty());
	}
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

//=============================================================================
// Slot_OneTouch_Highlight
//=============================================================================

BOOST_AUTO_TEST_CASE(Highlight_MarksTheCursorRowAndTracksIt)
{
	RenderFourRows();

	SendHighlight(2);

	BOOST_CHECK(device.DisplayedPage()[2].HighlightState == HighlightStates::Highlighted);
	BOOST_CHECK_EQUAL(static_cast<int>(device.HighlightedLineForTest()), 2);
}

BOOST_AUTO_TEST_CASE(Highlight_ClearAllSentinel_DropsTheCursorWithoutBeingOutOfRange)
{
	RenderFourRows();
	SendHighlight(2);
	BOOST_REQUIRE(device.DisplayedPage()[2].HighlightState == HighlightStates::Highlighted);

	// 0xFF is "clear all highlights", NOT an out-of-range line id.
	SendHighlight(0xFF);

	BOOST_CHECK(device.DisplayedPage()[2].HighlightState == HighlightStates::Normal);
	BOOST_CHECK_EQUAL(static_cast<int>(device.HighlightedLineForTest()), 0xFF);
}

BOOST_AUTO_TEST_CASE(Highlight_LineBeyondTheScreen_LeavesTheTrackedCursorAlone)
{
	RenderFourRows();
	SendHighlight(3);
	BOOST_REQUIRE_EQUAL(static_cast<int>(device.HighlightedLineForTest()), 3);

	// Line 12 is past the end of a 12-line screen (and is not the clear-all sentinel).
	SendHighlight(12);

	BOOST_CHECK_EQUAL(static_cast<int>(device.HighlightedLineForTest()), 3);
	// The screen updater rejects it too, so the previously highlighted row is untouched.
	BOOST_CHECK(device.DisplayedPage()[3].HighlightState == HighlightStates::Highlighted);
}

//=============================================================================
// Slot_OneTouch_HighlightChars
//=============================================================================

BOOST_AUTO_TEST_CASE(HighlightChars_MarksThePartialRangeOnTheRow)
{
	RenderFourRows();

	SendHighlightChars(1, 2, 7);

	const auto& row = device.DisplayedPage()[1];
	BOOST_CHECK(row.HighlightState == HighlightStates::PartiallyHighlighted);
	BOOST_REQUIRE(row.HighlightRange.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(row.HighlightRange.value().Start), 2);
	BOOST_CHECK_EQUAL(static_cast<int>(row.HighlightRange.value().Stop), 7);
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_CASE(HighlightChars_LineBeyondTheScreen_ChangesNothing)
{
	RenderFourRows();

	SendHighlightChars(12, 0, 4);

	for (std::size_t line = 0; line < device.DisplayedPage().Size(); ++line)
	{
		BOOST_CHECK(device.DisplayedPage()[line].HighlightState == HighlightStates::Normal);
	}
}

BOOST_AUTO_TEST_CASE(HighlightChars_InvertedRange_IsForwardedVerbatim)
{
	RenderFourRows();

	// start > stop is a malformed range: the router warns but still forwards it, so the row
	// records exactly what the controller sent rather than a silently "corrected" range.
	SendHighlightChars(1, 9, 3);

	const auto& row = device.DisplayedPage()[1];
	BOOST_CHECK(row.HighlightState == HighlightStates::PartiallyHighlighted);
	BOOST_REQUIRE(row.HighlightRange.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(row.HighlightRange.value().Start), 9);
	BOOST_CHECK_EQUAL(static_cast<int>(row.HighlightRange.value().Stop), 3);
}

//=============================================================================
// Slot_OneTouch_ShiftLines
//=============================================================================

BOOST_AUTO_TEST_CASE(ShiftLines_PositiveShift_ScrollsTheSpanDown)
{
	RenderFourRows();

	SendShiftLines(0, 3, 1);

	BOOST_CHECK(device.DisplayedPage()[0].Text.empty());
	BOOST_CHECK_EQUAL(device.DisplayedPage()[1].Text, std::string{ "Row0            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[2].Text, std::string{ "Row1            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[3].Text, std::string{ "Row2            " });
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_CASE(ShiftLines_NegativeShift_ScrollsTheSpanUp)
{
	RenderFourRows();

	SendShiftLines(0, 3, -1);

	BOOST_CHECK_EQUAL(device.DisplayedPage()[0].Text, std::string{ "Row1            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[1].Text, std::string{ "Row2            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[2].Text, std::string{ "Row3            " });
	BOOST_CHECK(device.DisplayedPage()[3].Text.empty());
}

BOOST_AUTO_TEST_CASE(ShiftLines_RangeBeyondTheScreen_ChangesNothing)
{
	RenderFourRows();

	SendShiftLines(0, 13, 1);

	BOOST_CHECK_EQUAL(device.DisplayedPage()[0].Text, std::string{ "Row0            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[3].Text, std::string{ "Row3            " });
}

BOOST_AUTO_TEST_CASE(ShiftLines_InvertedRange_ChangesNothing)
{
	RenderFourRows();

	SendShiftLines(5, 2, 1);

	BOOST_CHECK_EQUAL(device.DisplayedPage()[0].Text, std::string{ "Row0            " });
	BOOST_CHECK_EQUAL(device.DisplayedPage()[3].Text, std::string{ "Row3            " });
}

//=============================================================================
// Slot_OneTouch_DisplayUpdate / Slot_OneTouch_Unknown
//=============================================================================

BOOST_AUTO_TEST_CASE(DisplayUpdate_TicksTheControllerAndLeavesTheScreenAlone)
{
	RenderFourRows();

	Messages::JandyMessage_DisplayUpdate display_update;
	display_update.Signal_MessageWasReceived();

	BOOST_CHECK_EQUAL(device.DisplayedPage()[0].Text, std::string{ "Row0            " });
	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_CASE(UnknownMessage_TicksTheControllerRatherThanBeingDropped)
{
	BOOST_REQUIRE_EQUAL(OperatingState(), "ColdStart");

	// An undecoded frame addressed to us is still evidence the controller is alive.
	Messages::JandyMessage_Unknown unknown;
	unknown.Signal_MessageWasReceived();

	BOOST_CHECK_EQUAL(OperatingState(), "NormalOperation");
}

BOOST_AUTO_TEST_SUITE_END()
