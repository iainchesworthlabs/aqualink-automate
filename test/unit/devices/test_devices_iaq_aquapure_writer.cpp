#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/iaq/iaq_aquapure_writer.h"
#include "jandy/devices/iaq/iaq_command_sink.h"
#include "jandy/devices/iaq/iaq_page_model.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "kernel/body_of_water_ids.h"
#include "jandy/messages/iaq/iaq_message_page_button.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// AquaPureWriter -- the FAST chlorinator write path.
//
// The panel's own AquaPure page takes an ABSOLUTE output value; the OneTouch
// menu path can only step it 5% per key press. AqualinkD reaches it the same
// way (source/iaqtouch_aq_programmer.c).
//
// REGRESSION CONTEXT: the previous implementation queued four commands blindly
// and assumed the AquaPure page opened from a FIXED page-button index. The
// master lays every page out from the installed equipment, so no index is
// portable -- and 0x02 is the panel's single "Menu / Back" key, whose meaning
// depends on the screen it is pressed from. These cases pin the page-gated,
// label-driven behaviour that replaced it.
//=============================================================================

namespace
{
	constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
	constexpr uint8_t IAQ_PAGE_MENU{ 0x0f };
	constexpr uint8_t IAQ_PAGE_DEVICES{ 0x36 };
	constexpr uint8_t IAQ_PAGE_SET_SWG{ 0x30 };

	constexpr uint8_t CMD_MENU_OR_BACK{ 0x02 };
	// Confirmed against captures/iaq_chlorinator_set.cap: 0x19 from the menu opens page 0x30.
	constexpr uint8_t CMD_MENU_TO_AQUAPURE{ 0x19 };
	constexpr uint8_t CMD_SUBMIT{ 0x80 };
	constexpr uint8_t CMD_DWELL{ 0x00 };

	constexpr auto POOL = Kernel::BodyOfWaterIds::Pool;
	constexpr auto SPA = Kernel::BodyOfWaterIds::Spa;

	constexpr uint8_t PressFor(uint8_t button_index) { return static_cast<uint8_t>(0x11 + button_index); }

	// Records what the writer pushes at the poll-ACK channel.
	class RecordingSink : public IAQ::ICommandSink
	{
	public:
		void IssueCommand(uint8_t command) override { last = command; if (command != CMD_DWELL) { issued.push_back(command); } }
		void ArmControlValue(std::string value) override { control_value = std::move(value); }
		bool IsBusy() const override { return false; }

		uint8_t last{ CMD_DWELL };
		std::vector<uint8_t> issued;              // non-dwell commands, in order
		std::optional<std::string> control_value; // the armed control-data payload
	};

	IAQ::PageModel MakePage(uint8_t page_id, const std::vector<std::pair<uint8_t, std::string>>& buttons)
	{
		IAQ::PageModel page;
		page.OnPageStart(page_id);
		for (const auto& [index, name] : buttons)
		{
			page.UpsertButton(index, name, Messages::ButtonStatuses::Off);
		}
		return page;
	}

	// The writer emits at most one command per poll and dwells while the master renders, so a
	// test has to pump it. Runs until the goal completes or `max_polls` is exhausted.
	void Pump(IAQ::AquaPureWriter& writer, const IAQ::PageModel& page, RecordingSink& sink,
		const JandyDeviceType& device_id, int max_polls = 12)
	{
		for (int i = 0; (i < max_polls) && writer.HasPendingGoal(); ++i)
		{
			writer.ProcessStep(page, sink, device_id);
		}
	}

	struct AquaPureWriterFixture
	{
		AquaPureWriterFixture() : device_id(JandyDeviceId(0x33)) {}

		JandyDeviceType device_id;
		IAQ::AquaPureWriter writer;
		RecordingSink sink;
	};
}
// unnamed namespace

BOOST_FIXTURE_TEST_SUITE(IAQ_AquaPureWriter_TestSuite, AquaPureWriterFixture)

//-----------------------------------------------------------------------------
// Arming
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(NotEmulating_IsRefused)
{
	// A passive panel cannot transmit; refusing lets the dispatcher try another controller.
	BOOST_CHECK(writer.QueuePercentage(60, POOL, /*emulation_active=*/false, /*channel_busy=*/false, device_id)
		== Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(!writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(BusyChannel_IsRefused)
{
	BOOST_CHECK(writer.QueuePercentage(60, POOL, true, /*channel_busy=*/true, device_id)
		== Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(!writer.HasPendingGoal());
}

BOOST_AUTO_TEST_CASE(SecondGoalWhileOneIsInFlight_IsRefused)
{
	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	BOOST_CHECK(writer.QueuePercentage(30, POOL, true, false, device_id) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(OutOfRangePercentage_IsInvalid)
{
	BOOST_CHECK(writer.QueuePercentage(101, POOL, true, false, device_id) == Capabilities::ActuationResult::InvalidValue);
}

//-----------------------------------------------------------------------------
// Navigation
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(FromHome_PressesMenuKeyRatherThanGuessingAButton)
{
	const auto home = MakePage(IAQ_PAGE_HOME, { { 0, "Filter Pump" }, { 1, "Spa" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	writer.ProcessStep(home, sink, device_id);

	BOOST_REQUIRE_EQUAL(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued.front(), CMD_MENU_OR_BACK);
}

BOOST_AUTO_TEST_CASE(OnTheButtonlessMenu_PressesTheConfirmedAquaPureKey)
{
	// The menu page advertises NO buttons -- on a live bus (captures/iaq_chlorinator_set.cap) the
	// master sends PageStart(0x0f) followed straight by PageEnd. So this one hop cannot be
	// label-driven and must press the confirmed key, 0x19.
	const auto menu = MakePage(IAQ_PAGE_MENU, {});

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	writer.ProcessStep(menu, sink, device_id);

	BOOST_REQUIRE_EQUAL(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued.front(), CMD_MENU_TO_AQUAPURE);
}

BOOST_AUTO_TEST_CASE(OnMenu_PrefersALabelledEntryWhenThePanelAdvertisesOne)
{
	// Where a panel DOES advertise its menu buttons, the labelled entry wins over the blind key --
	// strictly better where available, free where not.
	const auto menu = MakePage(IAQ_PAGE_MENU, {
		{ 0, "Set Temperature" }, { 4, "System Setup" }, { 6, "Set AquaPure" }, { 7, "Set Time" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	writer.ProcessStep(menu, sink, device_id);

	BOOST_REQUIRE_EQUAL(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued.front(), PressFor(6));   // "Set AquaPure" matched on CONTAINS
}

BOOST_AUTO_TEST_CASE(MenuThatNeverOpensAquaPure_FailsAndLatchesRouteUnavailable)
{
	// Pressing and STAYING on the menu is what proves the route is absent -- not the button list,
	// which this page does not publish. The goal is abandoned after a bounded number of attempts
	// and every later request refuses up-front, so the dispatcher falls back to the OneTouch
	// menu crawl instead of re-walking here each time.
	const auto menu = MakePage(IAQ_PAGE_MENU, {});

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, menu, sink, device_id, 60);

	BOOST_CHECK(!writer.HasPendingGoal());
	BOOST_CHECK(writer.RouteUnavailable());
	BOOST_CHECK(!sink.control_value.has_value());   // nothing was ever submitted

	// And the next request refuses rather than walking the menu again.
	BOOST_CHECK(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::MappingFailed);
}

BOOST_AUTO_TEST_CASE(UnknownPage_DoesNotPressPageButtons)
{
	// On a page we do not recognise the writer only ever sends the Menu/Back key to unwind. It
	// must never press a page-button index on an unknown layout.
	const auto devices = MakePage(IAQ_PAGE_DEVICES, {
		{ 0, "Filter Pump" }, { 8, "Swim Jet" }, { 9, "Pool Light" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, devices, sink, device_id);

	for (const auto cmd : sink.issued)
	{
		BOOST_CHECK_EQUAL(cmd, CMD_MENU_OR_BACK);
	}
	BOOST_CHECK(!sink.control_value.has_value());
}

BOOST_AUTO_TEST_CASE(UnwindingIsBounded_TheGoalIsAbandonedNotRetriedForever)
{
	// The Menu/Back key is contextual, so it can fail to reach the menu. Bound it rather than
	// holding the shared command channel indefinitely.
	const auto devices = MakePage(IAQ_PAGE_DEVICES, { { 0, "Filter Pump" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, devices, sink, device_id, 200);

	BOOST_CHECK(!writer.HasPendingGoal());
}

//-----------------------------------------------------------------------------
// On the AquaPure page
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(OnTheAquaPurePage_SelectsPoolThenSubmitsTheAbsoluteValue)
{
	// This is the fast path: one press to pick the body, then the value goes across whole via
	// the control-data handshake -- no 5%-at-a-time stepping.
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Spa 30%" }, { 2, "Quick Boost" }, { 3, "Boost Setup" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id);

	BOOST_REQUIRE_GE(sink.issued.size(), 2u);
	BOOST_CHECK_EQUAL(sink.issued[0], PressFor(0));    // "Pool"
	BOOST_CHECK_EQUAL(sink.issued[1], CMD_SUBMIT);

	BOOST_REQUIRE(sink.control_value.has_value());
	BOOST_CHECK_EQUAL(sink.control_value.value(), std::string("160"));
}

BOOST_AUTO_TEST_CASE(WhenTheSpaIsCirculating_TheSpaRowIsSelected)
{
	// The panel chlorinates whichever body is circulating, so the goal follows the active body.
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Spa 30%" }, { 2, "Quick Boost" }, { 3, "Boost Setup" } });

	BOOST_REQUIRE(writer.QueuePercentage(45, SPA, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id);

	BOOST_REQUIRE_GE(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued[0], PressFor(1));    // "Spa"
	BOOST_REQUIRE(sink.control_value.has_value());
	BOOST_CHECK_EQUAL(sink.control_value.value(), std::string("145"));
}

BOOST_AUTO_TEST_CASE(SpaRequestOnAPoolOnlyPage_FailsRatherThanWritingThePoolRow)
{
	// Pool and spa are INDEPENDENT setpoints, so quietly writing the pool row when the spa was
	// asked for would change the wrong body and report success. Refuse instead.
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Quick Boost" } });

	BOOST_REQUIRE(writer.QueuePercentage(45, SPA, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id);

	BOOST_CHECK(!writer.HasPendingGoal());
	BOOST_CHECK(!sink.control_value.has_value());
	for (const auto cmd : sink.issued)
	{
		BOOST_CHECK_NE(cmd, PressFor(0));   // never pressed the Pool row
	}
}

BOOST_AUTO_TEST_CASE(PoolAndSpaAreIndependent_EachTargetsItsOwnRow)
{
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Pool 40%" }, { 1, "Spa 70%" }, { 2, "Quick Boost" } });

	// Pump to COMPLETION here (each hop settles for several polls while the master renders), so
	// the second goal can be armed -- one goal at a time on the shared command channel.
	BOOST_REQUIRE(writer.QueuePercentage(40, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id, 80);
	BOOST_REQUIRE(!writer.HasPendingGoal());
	BOOST_REQUIRE_GE(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued[0], PressFor(0));
	BOOST_CHECK_EQUAL(sink.control_value.value_or(""), std::string("140"));

	sink.issued.clear();
	sink.control_value.reset();

	BOOST_REQUIRE(writer.QueuePercentage(70, SPA, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id, 80);
	BOOST_REQUIRE_GE(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued[0], PressFor(1));
	BOOST_CHECK_EQUAL(sink.control_value.value_or(""), std::string("170"));
}

BOOST_AUTO_TEST_CASE(UnaddressableBody_IsInvalid)
{
	BOOST_CHECK(writer.QueuePercentage(50, Kernel::BodyOfWaterIds::Shared, true, false, device_id)
		== Capabilities::ActuationResult::InvalidValue);
}

BOOST_AUTO_TEST_CASE(AquaPurePageWithNoMatchingRow_FailsWithoutSubmitting)
{
	// Never guess a button: a page whose rows we cannot identify fails the goal outright rather
	// than firing an absolute value into an unknown field.
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Something Else" }, { 1, "Another Thing" } });

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id);

	BOOST_CHECK(!writer.HasPendingGoal());
	BOOST_CHECK(!sink.control_value.has_value());
}

BOOST_AUTO_TEST_CASE(Boost_PressesTheBoostRowAndSubmitsNoValue)
{
	// Boost is a toggle, not a value -- it must not arm the control-data handshake.
	const auto swg = MakePage(IAQ_PAGE_SET_SWG, { { 0, "Pool 30%" }, { 1, "Spa 30%" }, { 2, "Quick Boost" }, { 3, "Boost Setup" } });

	BOOST_REQUIRE(writer.QueueBoost(true, true, false, device_id) == Capabilities::ActuationResult::Accepted);
	Pump(writer, swg, sink, device_id);

	BOOST_REQUIRE_GE(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued[0], PressFor(2));
	BOOST_CHECK(!sink.control_value.has_value());
}

//-----------------------------------------------------------------------------
// Learning the route opportunistically
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ObserveMenuPage_LearnsTheRouteWithoutAGoal)
{
	// The menu renders for other reasons (the spa-switch writer walks through it), so the route
	// is learned for free whenever it does.
	const auto menu = MakePage(IAQ_PAGE_MENU, { { 4, "System Setup" }, { 6, "Set AquaPure" } });

	writer.ObserveMenuPage(menu);
	BOOST_CHECK(!writer.RouteUnavailable());

	BOOST_REQUIRE(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);

	// Straight to the learned button on arrival at the menu -- no re-discovery needed.
	writer.ProcessStep(menu, sink, device_id);
	BOOST_REQUIRE_EQUAL(sink.issued.size(), 1u);
	BOOST_CHECK_EQUAL(sink.issued.front(), PressFor(6));
}

BOOST_AUTO_TEST_CASE(ObserveMenuPage_IgnoresOtherPagesAndEmptyMenus)
{
	// A page that is not the menu, or a menu that has not rendered its buttons yet, proves
	// nothing either way -- it must not latch RouteUnavailable.
	writer.ObserveMenuPage(MakePage(IAQ_PAGE_DEVICES, { { 0, "Filter Pump" } }));
	BOOST_CHECK(!writer.RouteUnavailable());

	writer.ObserveMenuPage(MakePage(IAQ_PAGE_MENU, {}));
	BOOST_CHECK(!writer.RouteUnavailable());

	BOOST_CHECK(writer.QueuePercentage(60, POOL, true, false, device_id) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_SUITE_END()
