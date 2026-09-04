#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "navigation/menu_model.h"
#include "navigation/navigator.h"
#include "utility/screen_data_page.h"

using namespace AqualinkAutomate::Navigation;
using namespace AqualinkAutomate::Utility;

//=============================================================================
// Navigator -- the cursor-wrap, cursor-stuck, wait-timeout and recovery branches.
//
// The edge-case suite covers sync, transient pages, passwords and the stuck-
// recompute detector. These cases drive the remaining state-machine arms: a
// cursor that never reaches its row (wrap detection with and without a label
// retarget), a cursor pinned at a screen boundary (accepted for a page walk,
// fatal for an item walk), the wait-cycle timeout that kicks off recovery, and
// the two recovery strategies (Back presses, or a Select link to System on a
// page without Back).
//=============================================================================

namespace
{
	MenuModel BuildModel()
	{
		MenuModel model;

		// System (home) page: Menu/Help is reached by Select at line 11.
		MenuPage system_page;
		system_page.id = PageId::System;
		system_page.name = "System";
		system_page.detectors = { {0, "Equipment ON/OFF"} };
		system_page.edges = {
			{ EdgeTrigger::Select, PageId::System, PageId::MenuHelp, 11, "Menu/Help" },
			{ EdgeTrigger::Select, PageId::System, PageId::EquipmentOnOff, 1, "Filter Pump" },
			{ EdgeTrigger::LineUp, PageId::System, PageId::System, 0, "" },
			{ EdgeTrigger::LineDown, PageId::System, PageId::System, 0, "" }
		};
		model.RegisterPage(std::move(system_page));

		MenuPage help_page;
		help_page.id = PageId::MenuHelp;
		help_page.name = "Menu/Help";
		help_page.detectors = { {0, "Menu/Help"} };
		help_page.edges = {
			{ EdgeTrigger::Back, PageId::MenuHelp, PageId::System, 0, "" }
		};
		model.RegisterPage(std::move(help_page));

		// Equipment ON/OFF: has Back (so recovery presses Back from here).
		MenuPage equip_page;
		equip_page.id = PageId::EquipmentOnOff;
		equip_page.name = "Equipment ON/OFF";
		equip_page.detectors = { {0, "Equipment ON/OFF"}, {1, "Filter Pump"} };
		equip_page.edges = {
			{ EdgeTrigger::Back, PageId::EquipmentOnOff, PageId::System, 0, "" },
			{ EdgeTrigger::LineUp, PageId::EquipmentOnOff, PageId::EquipmentOnOff, 0, "" },
			{ EdgeTrigger::LineDown, PageId::EquipmentOnOff, PageId::EquipmentOnOff, 0, "" }
		};
		model.RegisterPage(std::move(equip_page));

		// OneTouch-style page: NO Back key, but a Select link to System (model says line 3).
		MenuPage onetouch_page;
		onetouch_page.id = PageId::OneTouch;
		onetouch_page.name = "OneTouch";
		onetouch_page.detectors = { {0, "OneTouch"} };
		onetouch_page.edges = {
			{ EdgeTrigger::Select, PageId::OneTouch, PageId::System, 3, "System" },
			{ EdgeTrigger::LineUp, PageId::OneTouch, PageId::OneTouch, 0, "" },
			{ EdgeTrigger::LineDown, PageId::OneTouch, PageId::OneTouch, 0, "" }
		};
		model.RegisterPage(std::move(onetouch_page));

		// Version: reachable only as an (unexpected) system-event landing page; Back -> System.
		MenuPage version_page;
		version_page.id = PageId::Version;
		version_page.name = "Version";
		version_page.detectors = { {0, "Version Info"} };
		version_page.edges = {
			{ EdgeTrigger::Back, PageId::Version, PageId::System, 0, "" }
		};
		model.RegisterPage(std::move(version_page));

		// Boost: registered but with NO incoming edge -> unreachable from anywhere.
		MenuPage boost_page;
		boost_page.id = PageId::Boost;
		boost_page.name = "Boost";
		boost_page.detectors = { {0, "Boost"} };
		model.RegisterPage(std::move(boost_page));

		MenuPage service_page;
		service_page.id = PageId::Service;
		service_page.name = "Service";
		service_page.detectors = { {0, "Service Mode"} };
		model.RegisterPage(std::move(service_page));

		MenuPage startup_page;
		startup_page.id = PageId::StartUp;
		startup_page.name = "StartUp";
		startup_page.detectors = { {7, "REV "} };
		startup_page.transient = true;
		model.RegisterPage(std::move(startup_page));

		model.RegisterGlobalEdge({ EdgeTrigger::SystemService, PageId::Unknown, PageId::Service, 0, "Service" });
		model.RegisterGlobalEdge({ EdgeTrigger::SystemTimeout, PageId::Unknown, PageId::Version, 0, "VersionEvent" });

		return model;
	}

	ScreenDataPage MakePage(const std::vector<std::pair<uint8_t, std::string>>& lines, size_t row_count = 12)
	{
		ScreenDataPage page(row_count);
		for (const auto& [line, text] : lines)
		{
			if (line < page.Size())
			{
				page[line].Text = text;
			}
		}
		return page;
	}

	// Sync the navigator onto `content` with the cursor on `cursor`.
	void SyncOn(Navigator& nav, const ScreenDataPage& content, uint8_t cursor)
	{
		nav.StartSync();
		for (uint32_t i = 0; i < Navigator::SYNC_REQUIRED_CONSISTENT_COUNT; ++i)
		{
			nav.OnPageUpdate(content, cursor);
		}
		BOOST_REQUIRE(nav.IsSynced());
	}

	int AsInt(Navigator::State state) { return static_cast<int>(state); }
	int AsInt(NavKeyCommand cmd) { return static_cast<int>(cmd); }
}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(Navigation_NavigatorBranchesTestSuite)

//-----------------------------------------------------------------------------
// Cursor stuck at a screen boundary
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CursorStuck_InItemMode_FailsAfterMaxAttempts)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto equip = MakePage({{0, "Equipment ON/OFF"}, {1, "Filter Pump"}});
	SyncOn(nav, equip, 0);

	// Fixed line 5, cursor pinned at 0: the item does not exist on this screen.
	nav.NavigateToItem(PageId::EquipmentOnOff, 5, "", PageId::Unknown);

	for (int i = 0; i < 4; ++i)
	{
		auto cmd = nav.OnPageUpdate(equip, 0);
		BOOST_REQUIRE(cmd.has_value());
		BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::LineDown));
		nav.OnStatusMessageReceived();
	}

	// The fifth consecutive non-move is the stuck threshold: an item walk fails cleanly.
	auto cmd = nav.OnPageUpdate(equip, 0);
	BOOST_CHECK(!cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::Failed));
	BOOST_CHECK(nav.IsComplete());
	BOOST_CHECK(!nav.IsSuccess());
}

BOOST_AUTO_TEST_CASE(CursorStuck_InPageWalk_AcceptsBoundaryAndSelects)
{
	auto model = BuildModel();
	Navigator nav(model);

	// No "Menu/Help" text on screen -> the model line (11) is the cursor target.
	auto system = MakePage({{0, "Equipment ON/OFF"}});
	SyncOn(nav, system, 0);

	nav.NavigateTo(PageId::MenuHelp);

	for (int i = 0; i < 4; ++i)
	{
		auto cmd = nav.OnPageUpdate(system, 0);
		BOOST_REQUIRE(cmd.has_value());
		BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::LineDown));
		nav.OnStatusMessageReceived();
	}

	// A page walk assumes it is as close as it can get and proceeds with the Select.
	auto cmd = nav.OnPageUpdate(system, 0);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
}

//-----------------------------------------------------------------------------
// Cursor wrap (moves but never arrives)
//-----------------------------------------------------------------------------

namespace
{
	// Drive MAX_CURSOR_MOVES cursor moves that alternate between two rows (so the cursor
	// is never "stuck") without ever reaching the target row.
	void WalkWithoutArriving(Navigator& nav, const ScreenDataPage& content)
	{
		for (uint32_t i = 0; i < Navigator::MAX_CURSOR_MOVES; ++i)
		{
			const uint8_t cursor = (i % 2 == 0) ? 1 : 2;
			auto cmd = nav.OnPageUpdate(content, cursor);
			BOOST_REQUIRE(cmd.has_value());
			BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::LineDown));
			nav.OnStatusMessageReceived();
		}
		BOOST_REQUIRE_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::MovingCursor));
	}
}

BOOST_AUTO_TEST_CASE(CursorWrap_WithLabelOnScreen_RetargetsAndSelects)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto system = MakePage({{0, "Equipment ON/OFF"}});
	SyncOn(nav, system, 1);

	nav.NavigateTo(PageId::MenuHelp);   // model line 11, label "Menu/Help"
	WalkWithoutArriving(nav, system);

	// On the wrap, the screen now shows the label on line 2 with the cursor already there:
	// content-based recovery retargets to line 2 and the Select goes out immediately.
	auto with_label = MakePage({{0, "Equipment ON/OFF"}, {2, "Menu/Help"}});
	auto cmd = nav.OnPageUpdate(with_label, 2);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
}

BOOST_AUTO_TEST_CASE(CursorWrap_WithoutLabelOnScreen_AcceptsPositionAndSelects)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto system = MakePage({{0, "Equipment ON/OFF"}});
	SyncOn(nav, system, 1);

	nav.NavigateTo(PageId::MenuHelp);
	WalkWithoutArriving(nav, system);

	// No label to retarget by: give up the move, accept the current row, and proceed.
	auto cmd = nav.OnPageUpdate(system, 2);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
}

BOOST_AUTO_TEST_CASE(CursorWrap_InItemMode_FailsNavigation)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto equip = MakePage({{0, "Equipment ON/OFF"}, {1, "Filter Pump"}});
	SyncOn(nav, equip, 1);

	// Fixed line 9 that the cursor never reaches (e.g. "AUX B1" on a panel without bank B).
	nav.NavigateToItem(PageId::EquipmentOnOff, 9, "", PageId::Unknown);
	WalkWithoutArriving(nav, equip);

	auto cmd = nav.OnPageUpdate(equip, 2);
	BOOST_CHECK(!cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::Failed));
}

BOOST_AUTO_TEST_CASE(MoveCursor_AboveTarget_SendsLineUp)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto equip = MakePage({{0, "Equipment ON/OFF"}, {1, "Filter Pump"}});
	SyncOn(nav, equip, 5);

	nav.NavigateToItem(PageId::EquipmentOnOff, 2, "", PageId::Unknown);

	auto cmd = nav.OnPageUpdate(equip, 5);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::LineUp));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::MovingCursor));
}

//-----------------------------------------------------------------------------
// Wait-cycle timeout -> recovery -> resume from home
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(WaitTimeout_InitiatesRecovery_ThenResumesFromHome)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto system = MakePage({{0, "Equipment ON/OFF"}});
	SyncOn(nav, system, 11);

	nav.NavigateTo(PageId::MenuHelp);
	auto cmd = nav.OnPageUpdate(system, 11);   // cursor already on line 11 -> Select
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_REQUIRE_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));

	// The controller never answers (no status messages): every update is a wasted wait cycle.
	for (uint32_t i = 0; i < Navigator::MAX_WAIT_CYCLES - 1; ++i)
	{
		BOOST_CHECK(!nav.OnPageUpdate(system, 11).has_value());
		BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
	}

	// The MAX_WAIT_CYCLES-th update times out and enters recovery (still waiting on the
	// pending status messages, so no command yet).
	BOOST_CHECK(!nav.OnPageUpdate(system, 11).has_value());
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::Reorienting));
	BOOST_CHECK(!nav.IsComplete());

	// Once the bus settles and we are (still) on the home page, recovery succeeds and the
	// navigation restarts from home: the Select is re-issued.
	nav.OnStatusMessageReceived();
	nav.OnStatusMessageReceived();
	cmd = nav.OnPageUpdate(system, 11);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
}

//-----------------------------------------------------------------------------
// Recovery strategies
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Recovery_PageWithoutBack_WalksCursorToSystemLinkThenSelects)
{
	auto model = BuildModel();
	Navigator nav(model);

	// The System link is on screen at line 2 (the model says 3): content resolution wins.
	auto onetouch = MakePage({{0, "OneTouch"}, {2, "System"}});
	SyncOn(nav, onetouch, 0);

	// Boost is unreachable -> no path -> recovery. This page has no Back key, so recovery
	// uses the Select link to System, first walking the cursor onto it.
	nav.NavigateTo(PageId::Boost);
	auto cmd = nav.OnPageUpdate(onetouch, 0);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::LineDown));
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::MovingCursor));

	// Cursor arrives on the System row: the next recovery pass presses Select.
	nav.OnStatusMessageReceived();
	cmd = nav.OnPageUpdate(onetouch, 2);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK(!nav.IsComplete());
}

BOOST_AUTO_TEST_CASE(Recovery_TooManyBackPresses_ExhaustsAttemptsAndFails)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto equip = MakePage({{0, "Equipment ON/OFF"}, {1, "Filter Pump"}});
	SyncOn(nav, equip, 0);

	nav.NavigateTo(PageId::Boost);   // unreachable -> recovery via Back presses

	// The controller keeps showing the same page after every Back: the navigator bounds the
	// presses per attempt (MAX_BACK_PRESSES) and the attempts overall (MAX_RECOVERY_ATTEMPTS).
	int back_presses = 0;
	for (int i = 0; (i < 40) && !nav.IsComplete(); ++i)
	{
		auto cmd = nav.OnPageUpdate(equip, 0);
		if (cmd.has_value())
		{
			BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Back));
			++back_presses;
		}
		nav.OnStatusMessageReceived();
		nav.OnStatusMessageReceived();
	}

	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::Failed));
	BOOST_CHECK_GE(back_presses, static_cast<int>(2 * Navigator::MAX_BACK_PRESSES));
	BOOST_CHECK_LT(back_presses, static_cast<int>(4 * Navigator::MAX_BACK_PRESSES));
}

BOOST_AUTO_TEST_CASE(UnexpectedPage_KnownSystemEventLanding_RecomputesPathFromThere)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto system = MakePage({{0, "Equipment ON/OFF"}});
	SyncOn(nav, system, 11);

	nav.NavigateTo(PageId::MenuHelp);
	auto cmd = nav.OnPageUpdate(system, 11);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));

	// Instead of Menu/Help the controller lands on the Version page (a registered
	// system-event target). The navigator recomputes from there: Version -> Back -> System.
	nav.OnStatusMessageReceived();
	nav.OnStatusMessageReceived();
	auto version = MakePage({{0, "Version Info"}});
	cmd = nav.OnPageUpdate(version, 0);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Back));
	BOOST_CHECK(nav.GetCurrentPage() == PageId::Version);
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));
}

//-----------------------------------------------------------------------------
// Sync ignores transient / special pages
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Sync_TransientAndServicePages_AreNotStartingPoints)
{
	auto model = BuildModel();
	Navigator nav(model);

	nav.StartSync();
	auto startup = MakePage({{6, "RS-8 Combo"}, {7, "REV T"}});
	auto service = MakePage({{0, "Service Mode"}});

	for (uint32_t i = 0; i < Navigator::SYNC_REQUIRED_CONSISTENT_COUNT + 1; ++i)
	{
		BOOST_CHECK(!nav.OnPageUpdate(startup, 0).has_value());
		BOOST_CHECK(!nav.OnPageUpdate(service, 0).has_value());
	}

	BOOST_CHECK(!nav.IsSynced());
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::Syncing));

	// A navigable page then syncs normally.
	auto system = MakePage({{0, "Equipment ON/OFF"}});
	for (uint32_t i = 0; i < Navigator::SYNC_REQUIRED_CONSISTENT_COUNT; ++i)
	{
		nav.OnPageUpdate(system, 0);
	}
	BOOST_CHECK(nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::System);
}

//-----------------------------------------------------------------------------
// Item navigation with a fixed line and a select target
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(NavigateToItem_EmptyLabelWithSelectTarget_SelectsAndArrives)
{
	auto model = BuildModel();
	Navigator nav(model);

	auto equip = MakePage({{0, "Equipment ON/OFF"}, {1, "Filter Pump"}});
	SyncOn(nav, equip, 5);

	// Cursor already on the fixed line 5 and a select target: press Select to enter it.
	nav.NavigateToItem(PageId::EquipmentOnOff, 5, "", PageId::MenuHelp);
	auto cmd = nav.OnPageUpdate(equip, 5);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK_EQUAL(AsInt(cmd.value()), AsInt(NavKeyCommand::Select));
	BOOST_CHECK(nav.GetTargetPage() == PageId::MenuHelp);
	BOOST_CHECK_EQUAL(AsInt(nav.GetState()), AsInt(Navigator::State::WaitingForPage));

	// The sub-page renders -> destination reached.
	nav.OnStatusMessageReceived();
	nav.OnStatusMessageReceived();
	auto help = MakePage({{0, "Menu/Help"}});
	cmd = nav.OnPageUpdate(help, 0);
	BOOST_CHECK(!cmd.has_value());
	BOOST_CHECK(nav.IsSuccess());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::MenuHelp);
}

BOOST_AUTO_TEST_SUITE_END()
