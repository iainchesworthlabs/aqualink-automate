#include <boost/test/unit_test.hpp>

#include "scheduling/controller_schedule.h"

#include "support/unit_test_onetouchdevice.h"

using namespace AqualinkAutomate;

//=============================================================================
// End-to-end (screen reconstruction + page processor): drive a per-equipment
// Program DETAIL page through the real OneTouch Screen/MessageLong/Status
// pipeline (LoadAndSignalTestPage) and assert the OneTouchDevice parsed it into
// the ControllerScheduleStore the /api/controller/schedules route serves.
//
// The Test::OneTouchDevice fixture registers a ControllerScheduleStore in its
// HubLocator (see unit_test_hublocatorinjector.cpp), which the device resolves in
// its constructor - so the scraped program has an observable sink.
//
// This also locks down the DETECTION FIX: the detail page's line 0 is the
// equipment name ("Filter Pump"), so the old { 0, "Program" } matcher missed it
// and it fell through to Page_EquipmentOnOff. The new { 2, "Pgm " } matcher routes
// it to PageProcessor_Program, and the aux processor harmlessly ignores every row.
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_ScheduleRead, Test::OneTouchDevice)

namespace
{
	Test::OneTouchDevice::TestPage FilterPumpProgramPage()
	{
		return {
			{ 0x0, "   Filter Pump    " },
			{ 0x1, "                  " },
			{ 0x2, "    Pgm 1 of 1    " },
			{ 0x3, " ON      11:00 AM " },
			{ 0x4, " OFF      2:00 PM " },
			{ 0x5, " All Days         " },
			{ 0x6, "                  " },
			{ 0x7, "                  " },
			{ 0x8, "                  " },
			{ 0x9, " Add      Program " },
			{ 0xA, " Delete   Program " },
			{ 0xB, " Change   Program " }
		};
	}

	Test::OneTouchDevice::TestPage SpaHeatProgramPage()
	{
		return {
			{ 0x0, "    Spa Heat      " },
			{ 0x1, "                  " },
			{ 0x2, "    Pgm 1 of 1    " },
			{ 0x3, " ON       6:00 AM " },
			{ 0x4, " OFF      9:30 AM " },
			{ 0x5, " Weekdays         " },
			{ 0x6, "                  " },
			{ 0x7, "                  " },
			{ 0x8, "                  " },
			{ 0x9, " Add      Program " },
			{ 0xA, " Delete   Program " },
			{ 0xB, " Change   Program " }
		};
	}
}

BOOST_AUTO_TEST_CASE(ProgramDetailPage_PopulatesControllerScheduleStore)
{
	auto store = this->Find<Scheduling::ControllerScheduleStore>();
	BOOST_REQUIRE(nullptr != store);

	// Precondition: nothing parsed yet.
	BOOST_CHECK(store->Status() == Scheduling::ControllerScheduleStatus::PendingCapture);
	BOOST_CHECK(store->List().empty());

	LoadAndSignalTestPage(FilterPumpProgramPage());

	BOOST_CHECK(store->Status() == Scheduling::ControllerScheduleStatus::Available);
	BOOST_REQUIRE_EQUAL(store->List().size(), 1u);

	const auto& s = store->List().front();
	BOOST_CHECK_EQUAL(s.target, "Filter Pump");
	BOOST_CHECK_EQUAL(s.on_hour, 11);
	BOOST_CHECK_EQUAL(s.on_minute, 0);
	BOOST_CHECK_EQUAL(s.off_hour, 14);
	BOOST_CHECK_EQUAL(s.off_minute, 0);
	BOOST_CHECK_EQUAL(s.days_of_week, 0x7f); // All Days
	BOOST_CHECK(s.enabled);
	BOOST_CHECK(!s.id.empty());              // stable per-slot id assigned
	BOOST_CHECK_EQUAL(s.name, s.target);
}

BOOST_AUTO_TEST_CASE(SuccessiveDetailPages_AccumulateInStore)
{
	auto store = this->Find<Scheduling::ControllerScheduleStore>();
	BOOST_REQUIRE(nullptr != store);

	// The OneTouch shows ONE program per detail page; visiting a second equipment's
	// page must ADD to the snapshot, not replace it.
	LoadAndSignalTestPage(FilterPumpProgramPage());
	BOOST_REQUIRE_EQUAL(store->List().size(), 1u);

	LoadAndSignalTestPage(SpaHeatProgramPage());
	BOOST_REQUIRE_EQUAL(store->List().size(), 2u);

	// Re-visiting the SAME page updates in place (keyed by target + index), not duplicates.
	LoadAndSignalTestPage(FilterPumpProgramPage());
	BOOST_CHECK_EQUAL(store->List().size(), 2u);

	// Both targets are present (map ordering: "Filter Pump" < "Spa Heat").
	bool saw_filter = false;
	bool saw_spa = false;
	for (const auto& s : store->List())
	{
		if (s.target == "Filter Pump") { saw_filter = true; }
		if (s.target == "Spa Heat")    { saw_spa = true; BOOST_CHECK_EQUAL(s.days_of_week, 0x1f); /* Weekdays */ }
	}
	BOOST_CHECK(saw_filter);
	BOOST_CHECK(saw_spa);
}

BOOST_AUTO_TEST_SUITE_END()
