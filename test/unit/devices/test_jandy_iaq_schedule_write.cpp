#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/signals2.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/devices/capabilities/actuation_types.h"
#include "jandy/messages/jandy_message_ack.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// iAQ controller-schedule WRITE (create a program). The emulated iAQ drives the
// AqualinkTouch Program pages: navigate to the Schedule list (0x28) -> Add
// Program (0x11) -> the device picker (0x38). RE'd from
// captures/iaq_schedule_{session,clean}.cap; see docs/iaq_schedule_protocol.md.
//
// This increment asserts the entry-point validation and the navigate -> Add
// Program emission, and that the writer fail-safes (abandons, no stray press) at
// the device-picker pick keystroke, which awaits a controlled picker capture.
//=============================================================================

namespace
{
	constexpr uint8_t IAQ_UI_ID = 0x33;
	constexpr uint8_t IAQ_PAGE_START = 0x23;
	constexpr uint8_t IAQ_PAGE_END = 0x28;
	constexpr uint8_t IAQ_TABLE_MESSAGE = 0x26;
	constexpr uint8_t IAQ_POLL = 0x30;
	constexpr uint8_t IAQ_PAGE_SCHEDULE_LIST = 0x28;
	constexpr uint8_t IAQ_PAGE_DEVICE_PICKER = 0x38;

	using Frame = std::vector<uint8_t>;

	Frame PageStart(uint8_t page_id) { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_PAGE_START, { page_id }); }
	Frame PageEnd() { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_PAGE_END, { 0x06, 0x0e }); }
	Frame Poll() { return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_POLL, { 0x00 }); }

	// A group-0 device-picker row: [0x00][attr][ label 0x00 ]. attr 0 = the "Devices" header.
	Frame DeviceRow(uint8_t attr, const std::string& label)
	{
		std::vector<uint8_t> data{ 0x00, attr };
		for (char c : label) { data.push_back(static_cast<uint8_t>(c)); }
		data.push_back(0x00);
		return Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_UI_ID, IAQ_TABLE_MESSAGE, data);
	}

	Scheduling::ControllerSchedule ValidProgram(const std::string& target = "Pool Light")
	{
		Scheduling::ControllerSchedule p;
		p.target = target;
		p.days_of_week = 0x01;   // Monday (a single, controller-expressible day)
		p.on_hour = 9;  p.on_minute = 0;
		p.off_hour = 17; p.off_minute = 0;
		return p;
	}

	void ReplayPicker(Test::MockReplayHarness& harness, const std::vector<std::string>& devices)
	{
		std::vector<Frame> frames;
		frames.push_back(PageStart(IAQ_PAGE_DEVICE_PICKER));
		frames.push_back(DeviceRow(0, "Devices"));
		uint8_t attr = 1;
		for (const auto& d : devices) { frames.push_back(DeviceRow(attr++, d)); }
		frames.push_back(PageEnd());
		harness.Replay(frames);
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_IAQ_ScheduleWrite)

//--- entry-point validation -------------------------------------------------

BOOST_AUTO_TEST_CASE(Create_NotEmulated_IsNotSupported)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/false);

	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(Create_NotRepresentable_IsInvalidValue)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	auto arbitrary_days = ValidProgram();
	arbitrary_days.days_of_week = 0x15;   // Mon+Wed+Fri -- the controller cannot represent it
	BOOST_CHECK(device.CreateControllerProgram(arbitrary_days) == Capabilities::ActuationResult::InvalidValue);

	auto no_target = ValidProgram("");
	BOOST_CHECK(device.CreateControllerProgram(no_target) == Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(Create_Valid_IsAccepted_ThenBusy)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::Accepted);
	// A second request while the first is in flight is rejected (busy panel -> NotSupported).
	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::NotSupported);
}

//--- closed-loop: navigate + Add Program, then fail-safe at the picker -------

BOOST_AUTO_TEST_CASE(Create_OnList_EmitsAddProgram_ThenAbandonsAtPicker)
{
	Test::MockReplayHarness harness;
	auto id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_UI_ID));
	IAQDevice device(id, harness.HubLocatorRef(), /*is_emulated=*/true);

	std::vector<uint8_t> cmds;
	boost::signals2::scoped_connection conn = Messages::JandyMessage_Ack::GetPublisher()->connect(
		[&cmds](std::reference_wrapper<const Messages::JandyMessage_Ack> r)
		{
			if (r.get().Command() != 0x00) { cmds.push_back(r.get().Command()); }
		});

	// Put the panel on the Schedule list, then request the write.
	harness.Replay({ PageStart(IAQ_PAGE_SCHEDULE_LIST), PageEnd() });
	BOOST_REQUIRE(device.CreateControllerProgram(ValidProgram("Pool Light")) == Capabilities::ActuationResult::Accepted);

	// Poll: NavigateToList (already on 0x28) -> AddProgram emits 0x11.
	for (int i = 0; (i < 20) && cmds.empty(); ++i) { harness.Replay({ Poll() }); }
	BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
	BOOST_CHECK_EQUAL(static_cast<int>(cmds[0]), 0x11);   // Add Program

	// The master renders the device picker with the target device visible (scrolled into view).
	ReplayPicker(harness, { "Filter Pump", "Spa", "Pool Heat", "Pool Light", "Spillway" });

	// The writer sees the target on-screen but the pick keystroke is pending a controlled capture,
	// so it abandons cleanly: no further command is emitted and the goal is released (idle again,
	// so a fresh request is accepted rather than Busy).
	const auto count_after_add = cmds.size();
	for (int i = 0; i < 10; ++i) { harness.Replay({ Poll() }); }
	BOOST_CHECK_EQUAL(cmds.size(), count_after_add);   // no stray/unverified keystroke
	BOOST_CHECK(device.CreateControllerProgram(ValidProgram()) == Capabilities::ActuationResult::Accepted);   // goal was released
}

BOOST_AUTO_TEST_SUITE_END()
