#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"

#include "scheduling/controller_schedule.h"

#include "support/unit_test_loadfixture.h"
#include "support/unit_test_mockreplayharness.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

namespace
{
	constexpr uint8_t IAQ_DEVICE_ID = 0x33; // AqualinkTouch address carrying the page UI.
}

//=============================================================================
// End-to-end: replay a real captured Schedule-list page (0x28, Group A) through
// the full decode stack and assert the IAQDevice populated the controller
// schedule store. Fixture extracted from a live iAQ capture (see
// captures/make_fixture.py; docs/iaq_schedule_protocol.md).
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_IAQ_ScheduleRead)

BOOST_AUTO_TEST_CASE(SchedulePage_PopulatesStore_FromLiveFixture)
{
	Test::MockReplayHarness harness;

	// The device resolves this store from the HubLocator in its constructor, so it
	// must be registered BEFORE the device is built (mirrors aqualink-automate.cpp,
	// where the store is registered ahead of Jandy::Configure).
	auto store = std::make_shared<Scheduling::ControllerScheduleStore>();
	harness.HubLocatorRef().Register<Scheduling::ControllerScheduleStore>(store);

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// Precondition: nothing parsed yet.
	BOOST_CHECK(store->Status() == Scheduling::ControllerScheduleStatus::PendingCapture);
	BOOST_CHECK(store->List().empty());

	// Frame-by-frame (each in its own read/Poll) -- a single flat Replay only drains
	// one read chunk per Poll and would truncate this 19-frame page mid-stream.
	Test::ReplayFixtureFramed(harness, "fixtures/iaq_schedule_group_a.cap");

	// The page carried five program entries for the active group "A".
	BOOST_CHECK(store->Status() == Scheduling::ControllerScheduleStatus::Available);
	BOOST_CHECK_EQUAL(store->ActiveGroup(), "A");
	BOOST_REQUIRE_EQUAL(store->List().size(), 5u);

	const auto& list = store->List();

	// Entry 1: Filter Pump, 11:00 AM -> 2:00 PM, all days.
	BOOST_CHECK_EQUAL(list[0].target, "Filter Pump");
	BOOST_CHECK_EQUAL(list[0].group, "A");
	BOOST_CHECK_EQUAL(list[0].on_hour, 11);
	BOOST_CHECK_EQUAL(list[0].on_minute, 0);
	BOOST_CHECK_EQUAL(list[0].off_hour, 14);
	BOOST_CHECK_EQUAL(list[0].days_of_week, 0x7f);
	BOOST_CHECK(list[0].enabled);
	BOOST_CHECK(!list[0].id.empty()); // stable per-slot id assigned

	// Entry 3: Pool Heat.
	BOOST_CHECK_EQUAL(list[2].target, "Pool Heat");

	// Entry 4: Solar Heat, 2:00 PM -> 11:00 AM (an overnight span: off < on).
	BOOST_CHECK_EQUAL(list[3].target, "Solar Heat");
	BOOST_CHECK_EQUAL(list[3].on_hour, 14);
	BOOST_CHECK_EQUAL(list[3].off_hour, 11);

	// Entry 5: Spillway.
	BOOST_CHECK_EQUAL(list[4].target, "Spillway");
}

BOOST_AUTO_TEST_SUITE_END()
