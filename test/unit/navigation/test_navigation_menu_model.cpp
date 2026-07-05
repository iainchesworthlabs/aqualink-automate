#include <boost/test/unit_test.hpp>

#include <set>

#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "navigation/menu_model.h"
#include "navigation/navigator.h"
#include "navigation/onetouch_menu_model.h"

using namespace AqualinkAutomate::Navigation;
using namespace AqualinkAutomate::Utility;

namespace
{
	// RAII helper: raise the Navigation channel to Trace for the scope of a test, then restore
	// the previous level. The deferred log-message lambdas in MenuModel (RegisterPage,
	// RegisterGlobalEdge, DetectPage best-match reporting, FindPath "no path") are only evaluated
	// when the channel's filter admits Trace, so exercising those formatting branches requires
	// lowering the filter first.
	struct ScopedNavigationTrace
	{
		AqualinkAutomate::Logging::Severity previous;
		ScopedNavigationTrace()
			: previous(AqualinkAutomate::Logging::SeverityFiltering::GetChannelFilterLevel(AqualinkAutomate::Logging::Channel::Navigation))
		{
			AqualinkAutomate::Logging::SeverityFiltering::SetChannelFilterLevel(
				AqualinkAutomate::Logging::Channel::Navigation, AqualinkAutomate::Logging::Severity::Trace);
		}
		~ScopedNavigationTrace()
		{
			AqualinkAutomate::Logging::SeverityFiltering::SetChannelFilterLevel(
				AqualinkAutomate::Logging::Channel::Navigation, previous);
		}
	};
}

// =============================================================================
// Helper: collect Select edges from a page
// =============================================================================
static std::vector<const MenuEdge*> GetSelectEdges(const MenuPage* page)
{
	std::vector<const MenuEdge*> result;
	if (!page) return result;
	for (const auto& edge : page->edges)
	{
		if (edge.trigger == EdgeTrigger::Select && edge.IsPageTransition())
		{
			result.push_back(&edge);
		}
	}
	return result;
}

BOOST_AUTO_TEST_SUITE(MenuModel_TestSuite)

// =============================================================================
// Edge Self-Loop Identification
// =============================================================================

BOOST_AUTO_TEST_CASE(TestEdgeSelfLoopIdentification)
{
	MenuEdge self_loop{ EdgeTrigger::LineUp, PageId::System, PageId::System, 0, "" };
	BOOST_CHECK(self_loop.IsSelfLoop());
	BOOST_CHECK(!self_loop.IsPageTransition());

	MenuEdge transition{ EdgeTrigger::Select, PageId::System, PageId::MenuHelp, 11, "Menu/Help" };
	BOOST_CHECK(!transition.IsSelfLoop());
	BOOST_CHECK(transition.IsPageTransition());
}

// =============================================================================
// BackTarget() Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestBackTargetDerivedFromBackEdges)
{
	auto model = CreateOneTouchMenuModel();

	// MenuHelp has Back edge to System
	const MenuPage* menu_page = model.GetPage(PageId::MenuHelp);
	BOOST_REQUIRE(menu_page != nullptr);
	auto back = menu_page->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::System);

	// SystemSetup has Back edge to MenuHelp
	const MenuPage* setup_page = model.GetPage(PageId::SystemSetup);
	BOOST_REQUIRE(setup_page != nullptr);
	back = setup_page->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::MenuHelp);

	// LabelAuxList has Back edge to SystemSetup
	const MenuPage* aux_list = model.GetPage(PageId::LabelAuxList);
	BOOST_REQUIRE(aux_list != nullptr);
	back = aux_list->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::SystemSetup);

	// LabelAux has Back edge to LabelAuxList
	const MenuPage* label_aux = model.GetPage(PageId::LabelAux);
	BOOST_REQUIRE(label_aux != nullptr);
	back = label_aux->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::LabelAuxList);

	// GeneralLabels has Back edge to LabelAux
	const MenuPage* general_labels = model.GetPage(PageId::GeneralLabels);
	BOOST_REQUIRE(general_labels != nullptr);
	back = general_labels->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::LabelAux);

}

BOOST_AUTO_TEST_CASE(TestBackTargetNulloptForOneTouchStylePages)
{
	auto model = CreateOneTouchMenuModel();

	// OneTouch page has no Back edge
	const MenuPage* onetouch_page = model.GetPage(PageId::OneTouch);
	BOOST_REQUIRE(onetouch_page != nullptr);
	BOOST_CHECK(!onetouch_page->BackTarget().has_value());

	// MoreOneTouch page has no Back edge
	const MenuPage* more_onetouch = model.GetPage(PageId::MoreOneTouch);
	BOOST_REQUIRE(more_onetouch != nullptr);
	BOOST_CHECK(!more_onetouch->BackTarget().has_value());

	// EquipmentOnOff page has no Back edge
	const MenuPage* equipment_page = model.GetPage(PageId::EquipmentOnOff);
	BOOST_REQUIRE(equipment_page != nullptr);
	BOOST_CHECK(!equipment_page->BackTarget().has_value());

	// System page has no Back edge (root)
	const MenuPage* system_page = model.GetPage(PageId::System);
	BOOST_REQUIRE(system_page != nullptr);
	BOOST_CHECK(!system_page->BackTarget().has_value());
}

// =============================================================================
// SupportsKey() Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSupportsKeyReturnsCorrectly)
{
	auto model = CreateOneTouchMenuModel();

	// OneTouch page: no Back, has Select/LineUp/LineDown
	const MenuPage* onetouch_page = model.GetPage(PageId::OneTouch);
	BOOST_REQUIRE(onetouch_page != nullptr);
	BOOST_CHECK(!onetouch_page->SupportsKey(EdgeTrigger::Back));
	BOOST_CHECK(onetouch_page->SupportsKey(EdgeTrigger::Select));
	BOOST_CHECK(onetouch_page->SupportsKey(EdgeTrigger::LineUp));
	BOOST_CHECK(onetouch_page->SupportsKey(EdgeTrigger::LineDown));

	// EquipmentOnOff: no Back, has Select/LineUp/LineDown/PageUp/PageDown
	const MenuPage* equipment_page = model.GetPage(PageId::EquipmentOnOff);
	BOOST_REQUIRE(equipment_page != nullptr);
	BOOST_CHECK(!equipment_page->SupportsKey(EdgeTrigger::Back));
	BOOST_CHECK(equipment_page->SupportsKey(EdgeTrigger::Select));
	BOOST_CHECK(equipment_page->SupportsKey(EdgeTrigger::PageUp));
	BOOST_CHECK(equipment_page->SupportsKey(EdgeTrigger::PageDown));

	// LabelAuxList: scrollable, has PageUp/PageDown
	const MenuPage* aux_list = model.GetPage(PageId::LabelAuxList);
	BOOST_REQUIRE(aux_list != nullptr);
	BOOST_CHECK(aux_list->SupportsKey(EdgeTrigger::PageUp));
	BOOST_CHECK(aux_list->SupportsKey(EdgeTrigger::PageDown));
	BOOST_CHECK(aux_list->SupportsKey(EdgeTrigger::Back));

	// MenuHelp: has Back
	const MenuPage* menu_page = model.GetPage(PageId::MenuHelp);
	BOOST_REQUIRE(menu_page != nullptr);
	BOOST_CHECK(menu_page->SupportsKey(EdgeTrigger::Back));
	BOOST_CHECK(menu_page->SupportsKey(EdgeTrigger::Select));
	BOOST_CHECK(menu_page->SupportsKey(EdgeTrigger::LineUp));
	BOOST_CHECK(menu_page->SupportsKey(EdgeTrigger::LineDown));

	// Service page: no edges at all
	const MenuPage* service_page = model.GetPage(PageId::Service);
	BOOST_REQUIRE(service_page != nullptr);
	BOOST_CHECK(!service_page->SupportsKey(EdgeTrigger::Back));
	BOOST_CHECK(!service_page->SupportsKey(EdgeTrigger::Select));
	BOOST_CHECK(!service_page->SupportsKey(EdgeTrigger::LineUp));
}

// =============================================================================
// Select Edge Line Number Tests (equivalent of old MenuItem line tests)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSystemPageSelectEdgeLineNumbers)
{
	auto model = CreateOneTouchMenuModel();
	const MenuPage* system_page = model.GetPage(PageId::System);
	BOOST_REQUIRE(system_page != nullptr);

	auto select_edges = GetSelectEdges(system_page);
	BOOST_REQUIRE_EQUAL(select_edges.size(), 3);

	// System page Select edges at lines 9-11
	BOOST_CHECK_EQUAL(select_edges[0]->label, "Equipment ON/OFF");
	BOOST_CHECK_EQUAL(select_edges[0]->trigger_line, 9);
	BOOST_CHECK(select_edges[0]->target == PageId::EquipmentOnOff);

	BOOST_CHECK_EQUAL(select_edges[1]->label, "OneTouch ON/OFF");
	BOOST_CHECK_EQUAL(select_edges[1]->trigger_line, 10);
	BOOST_CHECK(select_edges[1]->target == PageId::OneTouch);

	BOOST_CHECK_EQUAL(select_edges[2]->label, "Menu/Help");
	BOOST_CHECK_EQUAL(select_edges[2]->trigger_line, 11);
	BOOST_CHECK(select_edges[2]->target == PageId::MenuHelp);
}

BOOST_AUTO_TEST_CASE(TestMenuHelpPageSelectEdgeLineNumbers)
{
	auto model = CreateOneTouchMenuModel();
	const MenuPage* menu_page = model.GetPage(PageId::MenuHelp);
	BOOST_REQUIRE(menu_page != nullptr);

	auto select_edges = GetSelectEdges(menu_page);
	BOOST_REQUIRE_EQUAL(select_edges.size(), 11);

	// Menu/Help page Select edges starting at line 1 (line 0 is "Menu" title).
	// SetAquapure (line 5) and Boost Pool (line 11) are conditional chlorinator menu
	// items - reachable so the OneTouch can drive the chlorinator output / boost.
	BOOST_CHECK_EQUAL(select_edges[0]->label, "Help");
	BOOST_CHECK_EQUAL(select_edges[0]->trigger_line, 1);

	BOOST_CHECK_EQUAL(select_edges[1]->label, "Program");
	BOOST_CHECK_EQUAL(select_edges[1]->trigger_line, 2);
	BOOST_CHECK(select_edges[1]->target == PageId::Program);

	BOOST_CHECK_EQUAL(select_edges[2]->label, "Set Temp");
	BOOST_CHECK_EQUAL(select_edges[2]->trigger_line, 3);

	BOOST_CHECK_EQUAL(select_edges[3]->label, "Set Time");
	BOOST_CHECK_EQUAL(select_edges[3]->trigger_line, 4);

	BOOST_CHECK_EQUAL(select_edges[4]->label, "Set AquaPure");
	BOOST_CHECK_EQUAL(select_edges[4]->trigger_line, 5);
	BOOST_CHECK(select_edges[4]->target == PageId::SetAquapure);

	BOOST_CHECK_EQUAL(select_edges[5]->label, "Display Light");
	BOOST_CHECK_EQUAL(select_edges[5]->trigger_line, 6);
	BOOST_CHECK(select_edges[5]->target == PageId::DisplayLight);

	BOOST_CHECK_EQUAL(select_edges[6]->label, "Lockouts");
	BOOST_CHECK_EQUAL(select_edges[6]->trigger_line, 7);
	BOOST_CHECK(select_edges[6]->target == PageId::Lockouts);

	BOOST_CHECK_EQUAL(select_edges[7]->label, "Password");
	BOOST_CHECK_EQUAL(select_edges[7]->trigger_line, 8);
	BOOST_CHECK(select_edges[7]->target == PageId::PasswordSettings);

	BOOST_CHECK_EQUAL(select_edges[8]->label, "Program Group");
	BOOST_CHECK_EQUAL(select_edges[8]->trigger_line, 9);
	BOOST_CHECK(select_edges[8]->target == PageId::ProgramGroup);

	BOOST_CHECK_EQUAL(select_edges[9]->label, "System Setup");
	BOOST_CHECK_EQUAL(select_edges[9]->trigger_line, 10);
	BOOST_CHECK(select_edges[9]->target == PageId::SystemSetup);

	BOOST_CHECK_EQUAL(select_edges[10]->label, "Boost Pool");
	BOOST_CHECK_EQUAL(select_edges[10]->trigger_line, 11);
	BOOST_CHECK(select_edges[10]->target == PageId::Boost);
}

BOOST_AUTO_TEST_CASE(TestSystemSetupPageSelectEdgeLineNumbers)
{
	// CRITICAL: This test was added after fixing a bug where "Label Aux"
	// was incorrectly at line 1, causing navigation to select wrong items.
	// The first selectable item is at line 2 (line 0 = title, line 1 = blank)
	auto model = CreateOneTouchMenuModel();
	const MenuPage* setup_page = model.GetPage(PageId::SystemSetup);
	BOOST_REQUIRE(setup_page != nullptr);

	auto select_edges = GetSelectEdges(setup_page);
	BOOST_REQUIRE_GE(select_edges.size(), 1);

	// Label Aux should be at line 2, NOT line 1
	BOOST_CHECK_EQUAL(select_edges[0]->label, "Label Aux");
	BOOST_CHECK_EQUAL(select_edges[0]->trigger_line, 2);
	BOOST_CHECK(select_edges[0]->target == PageId::LabelAuxList);
}

BOOST_AUTO_TEST_CASE(TestLabelAuxListPageSelectEdgeLineNumbers)
{
	// CRITICAL: This test was added after fixing a bug where AUX items
	// started at line 1, but the first selectable item is at line 2.
	auto model = CreateOneTouchMenuModel();
	const MenuPage* aux_list_page = model.GetPage(PageId::LabelAuxList);
	BOOST_REQUIRE(aux_list_page != nullptr);

	auto select_edges = GetSelectEdges(aux_list_page);

	// Power center A aux outputs (Aux1-Aux7) plus the B/C/D power centers
	// (Aux B1-D8, 24 outputs) discovered by dynamic scrolling = 31 edges.
	BOOST_REQUIRE_EQUAL(select_edges.size(), 31);

	// Power center A items start at line 2 (line 0 = title, line 1 = blank) and
	// carry their default row labels so FindLineByLabel can resolve them.
	const char* primary_labels[] = { "Aux1", "Aux2", "Aux3", "Aux4", "Aux5", "Aux6", "Aux7" };
	for (size_t i = 0; i < 7; ++i)
	{
		BOOST_CHECK_EQUAL(select_edges[i]->label, primary_labels[i]);
		BOOST_CHECK_EQUAL(select_edges[i]->trigger_line, static_cast<uint8_t>(i + 2));
	}

	// Power center B/C/D items carry their default "Aux <X><n>" labels (used for
	// content-based discovery while scrolling) and UNIQUE synthetic trigger_line
	// hints (the multi-instance visited key is {source, trigger_line}).
	const char* scrolled_labels[] = {
		"Aux B1", "Aux B2", "Aux B3", "Aux B4", "Aux B5", "Aux B6", "Aux B7", "Aux B8",
		"Aux C1", "Aux C2", "Aux C3", "Aux C4", "Aux C5", "Aux C6", "Aux C7", "Aux C8",
		"Aux D1", "Aux D2", "Aux D3", "Aux D4", "Aux D5", "Aux D6", "Aux D7", "Aux D8"
	};
	for (size_t i = 0; i < 24; ++i)
	{
		BOOST_CHECK_EQUAL(select_edges[7 + i]->label, scrolled_labels[i]);
		// Synthetic monotonic hints start at 9 (just past the 8th visible line).
		BOOST_CHECK_EQUAL(select_edges[7 + i]->trigger_line, static_cast<uint8_t>(9 + i));
	}

	// Every trigger_line must be unique (the multi-instance visited key relies on
	// {source, trigger_line} to distinguish one aux edge from another).
	std::set<uint8_t> seen_lines;
	for (const auto* edge : select_edges)
	{
		BOOST_CHECK_MESSAGE(seen_lines.insert(edge->trigger_line).second,
			"Duplicate trigger_line " << static_cast<uint32_t>(edge->trigger_line)
			<< " on LabelAuxList Select edge");
	}

	// All AUX Select edges should target LabelAux page
	for (size_t i = 0; i < select_edges.size(); ++i)
	{
		BOOST_CHECK(select_edges[i]->target == PageId::LabelAux);
	}
}

BOOST_AUTO_TEST_CASE(TestHelpSubmenuPageSelectEdgeLineNumbers)
{
	auto model = CreateOneTouchMenuModel();
	const MenuPage* help_page = model.GetPage(PageId::HelpSubmenu);
	BOOST_REQUIRE(help_page != nullptr);

	auto select_edges = GetSelectEdges(help_page);
	BOOST_REQUIRE_EQUAL(select_edges.size(), 3);

	// Help submenu Select edges at lines 7-9 (line 0 = title, lines 1-3 = instructions, line 5 = Go Back)
	BOOST_CHECK_EQUAL(select_edges[0]->label, "Keys");
	BOOST_CHECK_EQUAL(select_edges[0]->trigger_line, 7);

	BOOST_CHECK_EQUAL(select_edges[1]->label, "Service");
	BOOST_CHECK_EQUAL(select_edges[1]->trigger_line, 8);

	BOOST_CHECK_EQUAL(select_edges[2]->label, "Diagnostics");
	BOOST_CHECK_EQUAL(select_edges[2]->trigger_line, 9);
}

BOOST_AUTO_TEST_CASE(TestOneTouchPageSelectEdges)
{
	// OneTouch page: Select toggles equipment on most lines.
	// Only "System" at line 11 is a navigation edge. "More OneTouch" removed
	// (scrolling, not Select navigation).
	auto model = CreateOneTouchMenuModel();
	const MenuPage* onetouch_page = model.GetPage(PageId::OneTouch);
	BOOST_REQUIRE(onetouch_page != nullptr);

	auto select_edges = GetSelectEdges(onetouch_page);
	BOOST_REQUIRE_EQUAL(select_edges.size(), 1);

	BOOST_CHECK_EQUAL(select_edges[0]->label, "System");
	BOOST_CHECK_EQUAL(select_edges[0]->trigger_line, 11);
	BOOST_CHECK(select_edges[0]->target == PageId::System);
}

BOOST_AUTO_TEST_CASE(TestSetTemperatureSubPageEdges)
{
	// SetTemperature: Pool Heat/Spa Heat are inline temperature edits,
	// not separate page transitions. No Select edges.
	auto model = CreateOneTouchMenuModel();
	const MenuPage* temp_page = model.GetPage(PageId::SetTemperature);
	BOOST_REQUIRE(temp_page != nullptr);

	auto select_edges = GetSelectEdges(temp_page);
	BOOST_CHECK_EQUAL(select_edges.size(), 0);
}

BOOST_AUTO_TEST_CASE(TestLabelAuxSubPageEdges)
{
	// LabelAux: 3 Select edges (General/Light/Waterfall).
	// CustomLabel removed (interactive character editor, dangerous to navigate into).
	auto model = CreateOneTouchMenuModel();
	const MenuPage* label_page = model.GetPage(PageId::LabelAux);
	BOOST_REQUIRE(label_page != nullptr);

	auto select_edges = GetSelectEdges(label_page);
	BOOST_REQUIRE_EQUAL(select_edges.size(), 3);

	BOOST_CHECK_EQUAL(select_edges[0]->label, "General Labels");
	BOOST_CHECK(select_edges[0]->target == PageId::GeneralLabels);

	// Note: edge label uses 3-space "Light   Labels" (column-aligned menu item text),
	// while the page title/detector uses 1-space "Light Labels".
	BOOST_CHECK_EQUAL(select_edges[1]->label, "Light   Labels");
	BOOST_CHECK(select_edges[1]->target == PageId::LightLabels);

	BOOST_CHECK_EQUAL(select_edges[2]->label, "Waterfall Labels");
	BOOST_CHECK(select_edges[2]->target == PageId::WaterfallLabels);
}

// =============================================================================
// Page Detection Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPageDetectionPatterns)
{
	auto model = CreateOneTouchMenuModel();

	// Verify key pages have detection patterns
	const MenuPage* system_page = model.GetPage(PageId::System);
	BOOST_REQUIRE(system_page != nullptr);
	BOOST_REQUIRE_GE(system_page->detectors.size(), 1);
	BOOST_CHECK_EQUAL(system_page->detectors[0].line, 9);
	BOOST_CHECK_EQUAL(system_page->detectors[0].pattern, "Equipment ON/OFF");

	const MenuPage* menu_page = model.GetPage(PageId::MenuHelp);
	BOOST_REQUIRE(menu_page != nullptr);
	BOOST_REQUIRE_GE(menu_page->detectors.size(), 1);
	BOOST_CHECK_EQUAL(menu_page->detectors[0].line, 0);
	BOOST_CHECK_EQUAL(menu_page->detectors[0].pattern, "Menu");

	const MenuPage* system_setup = model.GetPage(PageId::SystemSetup);
	BOOST_REQUIRE(system_setup != nullptr);
	BOOST_REQUIRE_GE(system_setup->detectors.size(), 1);
	BOOST_CHECK_EQUAL(system_setup->detectors[0].line, 0);
	BOOST_CHECK_EQUAL(system_setup->detectors[0].pattern, "System Setup");

	const MenuPage* label_aux_list = model.GetPage(PageId::LabelAuxList);
	BOOST_REQUIRE(label_aux_list != nullptr);
	BOOST_REQUIRE_GE(label_aux_list->detectors.size(), 1);
	BOOST_CHECK_EQUAL(label_aux_list->detectors[0].line, 0);
	BOOST_CHECK_EQUAL(label_aux_list->detectors[0].pattern, "Label Aux");
}

BOOST_AUTO_TEST_CASE(TestEnterPasswordPageDetection)
{
	// Verify Enter Password page is properly registered for detection
	auto model = CreateOneTouchMenuModel();
	const MenuPage* password_page = model.GetPage(PageId::EnterPassword);

	BOOST_REQUIRE(password_page != nullptr);
	BOOST_REQUIRE_GE(password_page->detectors.size(), 1);
	BOOST_CHECK_EQUAL(password_page->detectors[0].line, 0);
	BOOST_CHECK_EQUAL(password_page->detectors[0].pattern, "Enter Password");

	// Back edge target should be MenuHelp
	auto back = password_page->BackTarget();
	BOOST_REQUIRE(back.has_value());
	BOOST_CHECK(back.value() == PageId::MenuHelp);
}

// =============================================================================
// Pathfinding Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPathfindingSystemToLabelAuxList)
{
	auto model = CreateOneTouchMenuModel();

	// Path: System -> MenuHelp -> SystemSetup -> LabelAuxList
	auto path = model.FindPath(PageId::System, PageId::LabelAuxList);

	BOOST_REQUIRE(!path.empty());

	// First step: System -> MenuHelp (Select at line 11)
	bool found_menu_step = false;
	for (const auto* edge : path)
	{
		if (edge->source == PageId::System && edge->target == PageId::MenuHelp)
		{
			found_menu_step = true;
			BOOST_CHECK(edge->trigger == EdgeTrigger::Select);
			BOOST_CHECK_EQUAL(edge->trigger_line, 11);
		}
	}
	BOOST_CHECK(found_menu_step);
}

BOOST_AUTO_TEST_CASE(TestPathfindingWithCorrectLineNumbers)
{
	auto model = CreateOneTouchMenuModel();

	// Path: System -> MenuHelp -> SystemSetup
	auto path = model.FindPath(PageId::System, PageId::SystemSetup);

	BOOST_REQUIRE(!path.empty());

	// Find the MenuHelp -> SystemSetup step
	for (const auto* edge : path)
	{
		if (edge->source == PageId::MenuHelp && edge->target == PageId::SystemSetup)
		{
			BOOST_CHECK(edge->trigger == EdgeTrigger::Select);
			// System Setup is at line 10 in the menu
			BOOST_CHECK_EQUAL(edge->trigger_line, 10);
		}
	}
}

BOOST_AUTO_TEST_CASE(TestPathfindingFromSamePageIsEmpty)
{
	auto model = CreateOneTouchMenuModel();

	// No path needed when already at destination
	auto path = model.FindPath(PageId::System, PageId::System);
	BOOST_CHECK(path.empty());
}

BOOST_AUTO_TEST_CASE(TestPathfindingBetweenValidPagesSucceeds)
{
	auto model = CreateOneTouchMenuModel();

	// There should be a path from System to LabelAux
	auto path = model.FindPath(PageId::System, PageId::LabelAux);
	BOOST_CHECK(!path.empty());
}

BOOST_AUTO_TEST_CASE(TestPathfindingToNewPages)
{
	auto model = CreateOneTouchMenuModel();

	// Path from System to GeneralLabels (deep navigation)
	auto path = model.FindPath(PageId::System, PageId::GeneralLabels);
	BOOST_CHECK(!path.empty());

	// SetPoolHeat/SetSpaHeat are no longer registered pages (inline edits)
	// MoreOneTouch has no incoming edges (unreachable via navigation)

	// Path from System to Program
	path = model.FindPath(PageId::System, PageId::Program);
	BOOST_CHECK(!path.empty());

	// Path from System to DisplayLight
	path = model.FindPath(PageId::System, PageId::DisplayLight);
	BOOST_CHECK(!path.empty());
}

// =============================================================================
// Global System Event Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestGlobalSystemEventEdges)
{
	auto model = CreateOneTouchMenuModel();

	// System timeout event
	auto timeout_event = model.FindSystemEvent(PageId::TimeOut);
	BOOST_REQUIRE(timeout_event.has_value());
	BOOST_CHECK(timeout_event->trigger == EdgeTrigger::SystemTimeout);
	BOOST_CHECK(timeout_event->target == PageId::TimeOut);
	BOOST_CHECK_EQUAL(timeout_event->label, "Timeout");

	// System service event
	auto service_event = model.FindSystemEvent(PageId::Service);
	BOOST_REQUIRE(service_event.has_value());
	BOOST_CHECK(service_event->trigger == EdgeTrigger::SystemService);
	BOOST_CHECK(service_event->target == PageId::Service);
	BOOST_CHECK_EQUAL(service_event->label, "Service Mode");

	// No system event for normal pages
	auto no_event = model.FindSystemEvent(PageId::System);
	BOOST_CHECK(!no_event.has_value());
}

BOOST_AUTO_TEST_CASE(TestGlobalEdgesRegistered)
{
	auto model = CreateOneTouchMenuModel();
	const auto& global_edges = model.GetGlobalEdges();
	BOOST_REQUIRE_EQUAL(global_edges.size(), 2);
}

// =============================================================================
// Reachability Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestAllNonSpecialPagesReachableFromSystem)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	// Pages not reachable via normal menu navigation:
	// - Service/TimeOut: involuntary system events
	// - EquipmentStatus: shown by controller status change, not navigated to
	// - FreezeProtect: accessible via hardware buttons, not a menu item
	// - SelectSpeed: appears when selecting a variable-speed pump
	// - EnterPassword: appears when password-protected option is selected
	// - MoreOneTouch: reached via scrolling, not Select navigation
	// - CustomLabel: interactive character editor, edge removed to avoid edits
	// (SetAquapure + Boost ARE reachable - they carry chlorinator control - and so are
	// deliberately NOT excluded here; their reachability is asserted below.)
	std::set<PageId> special_pages = {
		PageId::Unknown,
		PageId::StartUp,
		PageId::Service,
		PageId::TimeOut,
		PageId::EquipmentStatus,
		PageId::FreezeProtect,
		PageId::SelectSpeed,
		PageId::EnterPassword,
		PageId::MoreOneTouch,
		PageId::CustomLabel,
	};

	for (const auto& [id, page] : pages)
	{
		if (special_pages.count(id) > 0)
		{
			continue;
		}

		if (id == PageId::System)
		{
			continue; // Can't navigate to self
		}

		auto path = model.FindPath(PageId::System, id);
		BOOST_CHECK_MESSAGE(!path.empty(),
			"No path from System to page '" + page.name + "' (id=" + std::to_string(static_cast<uint32_t>(id)) + ")");
	}
}

BOOST_AUTO_TEST_CASE(TestAllNonSpecialPagesReachableFromOneTouch)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	std::set<PageId> special_pages = {
		PageId::Unknown,
		PageId::StartUp,
		PageId::Service,
		PageId::TimeOut,
		PageId::EquipmentStatus,
		PageId::FreezeProtect,
		PageId::SelectSpeed,
		PageId::EnterPassword,
		PageId::MoreOneTouch,
		PageId::CustomLabel,
	};

	for (const auto& [id, page] : pages)
	{
		if (special_pages.count(id) > 0 || id == PageId::OneTouch)
		{
			continue;
		}

		auto path = model.FindPath(PageId::OneTouch, id);
		BOOST_CHECK_MESSAGE(!path.empty(),
			"No path from OneTouch to page '" + page.name + "' (id=" + std::to_string(static_cast<uint32_t>(id)) + ")");
	}
}

BOOST_AUTO_TEST_CASE(TestAllNonSpecialPagesReachableFromEquipmentOnOff)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	std::set<PageId> special_pages = {
		PageId::Unknown,
		PageId::StartUp,
		PageId::Service,
		PageId::TimeOut,
		PageId::EquipmentStatus,
		PageId::FreezeProtect,
		PageId::SelectSpeed,
		PageId::EnterPassword,
		PageId::MoreOneTouch,
		PageId::CustomLabel,
	};

	for (const auto& [id, page] : pages)
	{
		if (special_pages.count(id) > 0 || id == PageId::EquipmentOnOff)
		{
			continue;
		}

		auto path = model.FindPath(PageId::EquipmentOnOff, id);
		BOOST_CHECK_MESSAGE(!path.empty(),
			"No path from EquipmentOnOff to page '" + page.name + "' (id=" + std::to_string(static_cast<uint32_t>(id)) + ")");
	}
}

// =============================================================================
// Capability-gated page classification (used to label post-crawl survey failures
// as expected-absent vs notable on a given controller model).
// =============================================================================

BOOST_AUTO_TEST_CASE(TestCapabilityRequirement_IAQPages_RequireIAqualink)
{
	BOOST_CHECK(OneTouchPageCapabilityRequirement(PageId::DiagnosticsIAQStatus).has_value());
	BOOST_CHECK(OneTouchPageCapabilityRequirement(PageId::DiagnosticsIAQRSSI).has_value());
}

BOOST_AUTO_TEST_CASE(TestCapabilityRequirement_ChlorinatorPages_RequireSWG)
{
	BOOST_CHECK(OneTouchPageCapabilityRequirement(PageId::Boost).has_value());
	BOOST_CHECK(OneTouchPageCapabilityRequirement(PageId::SetAquapure).has_value());
}

BOOST_AUTO_TEST_CASE(TestCapabilityRequirement_CorePages_HaveNoRequirement)
{
	// Pages every panel has must NOT be classified as capability-gated, so a failure to
	// reach them is reported as a notable problem rather than silently excused.
	BOOST_CHECK(!OneTouchPageCapabilityRequirement(PageId::EquipmentOnOff).has_value());
	BOOST_CHECK(!OneTouchPageCapabilityRequirement(PageId::System).has_value());
	BOOST_CHECK(!OneTouchPageCapabilityRequirement(PageId::SetTemperature).has_value());
	BOOST_CHECK(!OneTouchPageCapabilityRequirement(PageId::LabelAuxList).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Navigator Integration Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(Navigator_TestSuite)

BOOST_AUTO_TEST_CASE(TestMenuModelAllPagesRegistered)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	// 28 pages registered (15 original + 12 new in Phase 2 + 1 StartUp)
	BOOST_CHECK_GE(pages.size(), 28);

	// Verify critical pages exist (original)
	BOOST_CHECK(model.GetPage(PageId::System) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::OneTouch) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::MenuHelp) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::SystemSetup) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::LabelAuxList) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::LabelAux) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::EquipmentOnOff) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::EnterPassword) != nullptr);

	// Verify new Phase 2 pages exist
	// Note: SetPoolHeat/SetSpaHeat are no longer registered (inline temperature edits)
	BOOST_CHECK(model.GetPage(PageId::MoreOneTouch) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::Program) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::DisplayLight) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::Lockouts) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::PasswordSettings) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::ProgramGroup) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::GeneralLabels) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::LightLabels) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::WaterfallLabels) != nullptr);
	BOOST_CHECK(model.GetPage(PageId::CustomLabel) != nullptr);
}

BOOST_AUTO_TEST_CASE(TestEveryPageHasAtLeastOneDetector)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	for (const auto& [id, page] : pages)
	{
		if (id == PageId::Unknown)
		{
			continue; // Unknown page has no detectors
		}

		BOOST_CHECK_MESSAGE(!page.detectors.empty(),
			"Page '" + page.name + "' (id=" + std::to_string(static_cast<uint32_t>(id)) + ") has no detectors");
	}
}

BOOST_AUTO_TEST_CASE(TestEveryNonSpecialPageHasLineUpDownSelfLoops)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	// Special pages with no LineUp/LineDown edges
	std::set<PageId> no_edge_pages = {
		PageId::Unknown,
		PageId::StartUp,
		PageId::Service,
		PageId::TimeOut,
		PageId::CustomLabel,  // Only has Back edge (interactive editor)
	};

	for (const auto& [id, page] : pages)
	{
		if (no_edge_pages.count(id) > 0)
		{
			continue;
		}

		BOOST_CHECK_MESSAGE(page.SupportsKey(EdgeTrigger::LineUp),
			"Page '" + page.name + "' missing LineUp self-loop edge");
		BOOST_CHECK_MESSAGE(page.SupportsKey(EdgeTrigger::LineDown),
			"Page '" + page.name + "' missing LineDown self-loop edge");
	}
}

BOOST_AUTO_TEST_CASE(TestNoDuplicatePageTypes)
{
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	// Check that no two registered pages share the same ScreenDataPageTypes
	// (except Page_Unknown which may be shared)
	std::set<ScreenDataPageTypes> seen_types;
	for (const auto& [id, page] : pages)
	{
		if (page.page_type == ScreenDataPageTypes::Page_Unknown)
		{
			continue;
		}

		BOOST_CHECK_MESSAGE(seen_types.find(page.page_type) == seen_types.end(),
			"Duplicate ScreenDataPageTypes found for page '" + page.name + "'");
		seen_types.insert(page.page_type);
	}
}

// =============================================================================
// FindPath returns edge pointers
// =============================================================================

BOOST_AUTO_TEST_CASE(TestFindPathReturnsEdgePointers)
{
	auto model = CreateOneTouchMenuModel();

	// Path from System to LabelAuxList should contain edge pointers
	auto path = model.FindPath(PageId::System, PageId::LabelAuxList);
	BOOST_REQUIRE(!path.empty());

	// Every element should be a valid edge pointer with consistent source->target chain
	for (size_t i = 0; i < path.size(); ++i)
	{
		BOOST_REQUIRE(path[i] != nullptr);
		BOOST_CHECK(path[i]->IsPageTransition());

		// Each step's source should match the previous step's target (or System for first step)
		if (i == 0)
		{
			BOOST_CHECK(path[i]->source == PageId::System);
		}
		else
		{
			BOOST_CHECK(path[i]->source == path[i - 1]->target);
		}
	}

	// Last step should end at LabelAuxList
	BOOST_CHECK(path.back()->target == PageId::LabelAuxList);
}

BOOST_AUTO_TEST_CASE(TestEdgePointersAccessLabelAndTriggerLine)
{
	auto model = CreateOneTouchMenuModel();

	// Path from System to MenuHelp
	auto path = model.FindPath(PageId::System, PageId::MenuHelp);
	BOOST_REQUIRE_EQUAL(path.size(), 1);

	// Edge should have label "Menu/Help" and trigger_line 11
	BOOST_CHECK_EQUAL(path[0]->label, "Menu/Help");
	BOOST_CHECK_EQUAL(path[0]->trigger_line, 11);
	BOOST_CHECK(path[0]->trigger == EdgeTrigger::Select);
}

// =============================================================================
// GetIncomingSelectEdges: cached index correctness + invalidation on registration
// =============================================================================

BOOST_AUTO_TEST_CASE(TestGetIncomingSelectEdgesCacheMatchesFreshScan)
{
	MenuModel model;

	MenuPage system_page;
	system_page.id = PageId::System;
	system_page.name = "System";
	system_page.edges = {
		{ EdgeTrigger::Select, PageId::System, PageId::MenuHelp, 11, "Menu/Help" }
	};
	model.RegisterPage(std::move(system_page));

	MenuPage help_page;
	help_page.id = PageId::MenuHelp;
	help_page.name = "Menu/Help";
	help_page.edges = {
		{ EdgeTrigger::Select, PageId::MenuHelp, PageId::SystemSetup, 3, "System Setup" }
	};
	model.RegisterPage(std::move(help_page));

	// First query populates the lazy index.
	auto into_help = model.GetIncomingSelectEdges(PageId::MenuHelp);
	BOOST_REQUIRE_EQUAL(into_help.size(), 1);
	BOOST_CHECK(into_help[0]->source == PageId::System);
	BOOST_CHECK(into_help[0]->target == PageId::MenuHelp);

	// A page with no incoming Select edges returns empty.
	BOOST_CHECK(model.GetIncomingSelectEdges(PageId::System).empty());

	// Registering a NEW page with another incoming Select edge must invalidate the cache so the
	// next query reflects it (rather than returning the stale memoised result).
	MenuPage setup_page;
	setup_page.id = PageId::SystemSetup;
	setup_page.name = "System Setup";
	setup_page.edges = {
		{ EdgeTrigger::Select, PageId::SystemSetup, PageId::MenuHelp, 0, "Back to Help" }
	};
	model.RegisterPage(std::move(setup_page));

	auto into_help_after = model.GetIncomingSelectEdges(PageId::MenuHelp);
	BOOST_CHECK_EQUAL(into_help_after.size(), 2);
}

// =============================================================================
// Navigator Syncing State Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestNavigatorStartSyncEntersSyncingState)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);
	BOOST_CHECK(!nav.IsSynced());

	nav.StartSync();

	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);
	BOOST_CHECK(!nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::Unknown);
}

BOOST_AUTO_TEST_CASE(TestNavigatorSyncRequiresConsistentDetections)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	nav.StartSync();

	// Build a page that matches the System page detector (line 9: "Equipment ON/OFF")
	ScreenDataPage page(12);
	page[9].Text = "Equipment ON/OFF";

	// First detection - not yet synced
	nav.OnPageUpdate(page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);
	BOOST_CHECK(!nav.IsSynced());

	// Second detection - still not enough
	nav.OnPageUpdate(page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);
	BOOST_CHECK(!nav.IsSynced());

	// Third detection - should complete sync (SYNC_REQUIRED_CONSISTENT_COUNT = 3)
	nav.OnPageUpdate(page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);
	BOOST_CHECK(nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::System);
}

BOOST_AUTO_TEST_CASE(TestNavigatorSyncResetsOnDifferentPage)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	nav.StartSync();

	// System page detected twice
	ScreenDataPage system_page(12);
	system_page[9].Text = "Equipment ON/OFF";

	nav.OnPageUpdate(system_page, 0);
	nav.OnPageUpdate(system_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);

	// Now a different page is detected - counter should reset
	ScreenDataPage onetouch_page(12);
	onetouch_page[11].Text = "System";

	nav.OnPageUpdate(onetouch_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);

	// Need 3 consistent detections of the new page
	nav.OnPageUpdate(onetouch_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);

	nav.OnPageUpdate(onetouch_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);
	BOOST_CHECK(nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::OneTouch);
}

BOOST_AUTO_TEST_CASE(TestNavigatorSyncResetsOnUnknownPage)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	nav.StartSync();

	// System page detected twice
	ScreenDataPage system_page(12);
	system_page[9].Text = "Equipment ON/OFF";

	nav.OnPageUpdate(system_page, 0);
	nav.OnPageUpdate(system_page, 0);

	// Unknown page (empty content) resets counter
	ScreenDataPage empty_page(12);
	nav.OnPageUpdate(empty_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);

	// Need 3 fresh consistent detections
	nav.OnPageUpdate(system_page, 0);
	nav.OnPageUpdate(system_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Syncing);

	nav.OnPageUpdate(system_page, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);
	BOOST_CHECK(nav.GetCurrentPage() == PageId::System);
}

BOOST_AUTO_TEST_CASE(TestNavigatorResetClearsSyncState)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	// Sync to a page
	nav.StartSync();
	ScreenDataPage page(12);
	page[9].Text = "Equipment ON/OFF";
	nav.OnPageUpdate(page, 0);
	nav.OnPageUpdate(page, 0);
	nav.OnPageUpdate(page, 0);
	BOOST_CHECK(nav.IsSynced());

	// Reset clears everything
	nav.Reset();
	BOOST_CHECK(nav.GetState() == Navigator::State::Idle);
	BOOST_CHECK(!nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::Unknown);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// StartUp Page Detection with max_content_lines Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(StartUpDetection_TestSuite)

BOOST_AUTO_TEST_CASE(TestStartUpPageDetectionWithSparseContent)
{
	auto model = CreateOneTouchMenuModel();

	// Build a 12-line page with only 3 populated lines (typical cold start splash)
	// Line 4: model number, Line 5: panel type (contains "-"), Line 7: revision (contains "REV ")
	ScreenDataPage page(12);
	page[4].Text = "AquaLink PDA";
	page[5].Text = "PD-PS-8";
	page[7].Text = "REV MMM";

	// 3 non-empty lines <= max_content_lines (4), so StartUp should match
	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::StartUp);
}

BOOST_AUTO_TEST_CASE(TestStartUpPageRejectedWhenTooManyLines)
{
	auto model = CreateOneTouchMenuModel();

	// Build a page that matches StartUp detectors but has too many populated lines
	// (simulating the HelpService/Version page which has many more lines of content)
	ScreenDataPage page(12);
	page[0].Text = "Model: AQL-PS-8";
	page[1].Text = "Pool Sensors";
	page[2].Text = "Air: 72F  Pool: 78F";
	page[3].Text = "Spa: 80F";
	page[4].Text = "AquaLink PDA";
	page[5].Text = "PD-PS-8";
	page[6].Text = "Serial: 12345";
	page[7].Text = "REV MMM";

	// 8 non-empty lines > max_content_lines (4), so StartUp should NOT match
	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected != PageId::StartUp);
}

BOOST_AUTO_TEST_CASE(TestStartUpPageMaxContentLinesIsSet)
{
	auto model = CreateOneTouchMenuModel();

	// Verify the StartUp page has max_content_lines configured
	const MenuPage* startup_page = model.GetPage(PageId::StartUp);
	BOOST_REQUIRE(startup_page != nullptr);
	BOOST_CHECK(startup_page->max_content_lines.has_value());
	BOOST_CHECK_EQUAL(startup_page->max_content_lines.value(), 4);
}

BOOST_AUTO_TEST_CASE(TestHelpServicePageNoMaxContentLines)
{
	auto model = CreateOneTouchMenuModel();

	// Verify the HelpService page does NOT have max_content_lines set
	const MenuPage* help_service = model.GetPage(PageId::HelpService);
	BOOST_REQUIRE(help_service != nullptr);
	BOOST_CHECK(!help_service->max_content_lines.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// DetectPage Algorithm Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(DetectPage_TestSuite)

BOOST_AUTO_TEST_CASE(TestDetectPageMatchesSingleDetector)
{
	auto model = CreateOneTouchMenuModel();

	// System page has a single detector: line 9 = "Equipment ON/OFF"
	ScreenDataPage page(12);
	page[9].Text = "Equipment ON/OFF";

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::System);
}

BOOST_AUTO_TEST_CASE(TestDetectPageMatchesMultipleDetectors)
{
	auto model = CreateOneTouchMenuModel();

	// HelpSubmenu requires both line 0 = "Help" AND line 7 = "Keys"
	ScreenDataPage page(12);
	page[0].Text = "Help";
	page[7].Text = "Keys";

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::HelpSubmenu);
}

BOOST_AUTO_TEST_CASE(TestDetectPageRejectsPartialDetectorMatch)
{
	auto model = CreateOneTouchMenuModel();

	// Only line 0 = "Help" without line 7 = "Keys" should match MenuHelp, not HelpSubmenu
	ScreenDataPage page(12);
	page[0].Text = "Help";
	// line 7 left empty — HelpSubmenu's second detector won't match

	auto detected = model.DetectPage(page);
	// Should NOT detect as HelpSubmenu (needs both detectors)
	BOOST_CHECK(detected != PageId::HelpSubmenu);
}

BOOST_AUTO_TEST_CASE(TestDetectPageTiebreakerMostDetectorsWins)
{
	auto model = CreateOneTouchMenuModel();

	// SetTime has 2 detectors: {{ 0, "Set Time" }, { 7, "Use Arrow Keys" }}
	// CustomLabel has 1 detector: {{ 7, "Use Arrow Keys" }}
	// When both patterns are present, SetTime (2 detectors) should win.
	ScreenDataPage page(12);
	page[0].Text = "Set Time";
	page[7].Text = "Use Arrow Keys";

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::SetTime);
}

BOOST_AUTO_TEST_CASE(TestDetectPageTiebreakerLongestPatternWins)
{
	auto model = CreateOneTouchMenuModel();

	// When two single-detector pages both match, the one with the longer
	// pattern should win. MenuHelp (line 0 "Menu", len=4) vs Program
	// (line 0 "Program", len=7). But they match different content.
	// Let's use a concrete case: EquipmentOnOff (line 0 "Filter Pump", len=11)
	// won't conflict with System (line 9 "Equipment ON/OFF", len=16)
	// because they use different lines. Instead, verify the mechanism:
	// build content matching only one page to confirm specificity.

	// DiagnosticsIAQStatus (line 0, "iAquaLink Status", len=16)
	// vs DiagnosticsIAQRSSI (line 0, "iAquaLink RSSI", len=14)
	// These are on the same line but different patterns - no overlap.
	// Instead, test that a page with 1 detector of length N beats another
	// with 1 detector of length M < N when both match.

	// "iAquaLink Status" contains "iAquaLink" as substring.
	// DiagnosticsIAQStatus: {{ 0, "iAquaLink Status" }} (len=16)
	// If there was a page with {{ 0, "iAquaLink" }} (len=9), it would lose.
	// Since we can't easily create that scenario with the real model,
	// we just verify the priority mechanism works for SetTime vs CustomLabel.
	ScreenDataPage page(12);
	page[0].Text = "Set Time";
	page[7].Text = "Use Arrow Keys to change letter";  // Contains "Use Arrow Keys"

	auto detected = model.DetectPage(page);
	// SetTime has 2 detectors (both match): total pattern_len = 8 + 14 = 22
	// CustomLabel has 1 detector (matches): total pattern_len = 14
	// 2 detectors > 1 detector -> SetTime wins
	BOOST_CHECK(detected == PageId::SetTime);
}

BOOST_AUTO_TEST_CASE(TestDetectPageReturnsUnknownForEmptyContent)
{
	auto model = CreateOneTouchMenuModel();

	ScreenDataPage page(12);
	// All lines empty

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::Unknown);
}

BOOST_AUTO_TEST_CASE(TestDetectPageRejectsDetectorOnWrongLine)
{
	auto model = CreateOneTouchMenuModel();

	// System page detector expects "Equipment ON/OFF" on line 9.
	// Put it on line 0 instead - should NOT match System.
	ScreenDataPage page(12);
	page[0].Text = "Equipment ON/OFF";

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected != PageId::System);
}

BOOST_AUTO_TEST_CASE(TestDetectPageSubstringMatching)
{
	auto model = CreateOneTouchMenuModel();

	// DetectPage uses find() (substring match), so padded text matches.
	// "  Set Time  " should match detector pattern "Set Time".
	ScreenDataPage page(12);
	page[0].Text = "  Set Time  ";
	page[7].Text = "  Use Arrow Keys to change letter  ";

	auto detected = model.DetectPage(page);
	BOOST_CHECK(detected == PageId::SetTime);
}

BOOST_AUTO_TEST_CASE(TestTraceLoggingBranchesAreExercised_DetectAndNoPath)
{
	// With the Navigation channel raised to Trace, the deferred log-message lambdas along the
	// register / detect / pathfind paths are actually evaluated. This drives those formatting
	// branches while still asserting the real observable results (a correct detection and an
	// empty path). Behaviour must be identical to the Info-filtered case.
	ScopedNavigationTrace trace_guard;

	MenuModel model;

	// Register a page (RegisterPage's trace lambda) with a single detector...
	MenuPage system;
	system.id = PageId::System;
	system.name = "System";
	system.page_type = ScreenDataPageTypes::Page_Unknown;
	system.detectors.push_back(Detector{ 9, "Equipment ON/OFF" });
	model.RegisterPage(std::move(system));

	// ...and a second, unconnected page so a path query between them can genuinely fail.
	MenuPage isolated;
	isolated.id = PageId::OneTouch;
	isolated.name = "OneTouch";
	isolated.page_type = ScreenDataPageTypes::Page_Unknown;
	isolated.detectors.push_back(Detector{ 0, "OneTouch" });
	model.RegisterPage(std::move(isolated));

	// Register a global edge (RegisterGlobalEdge's trace lambda).
	model.RegisterGlobalEdge(MenuEdge{ EdgeTrigger::SystemTimeout, PageId::Unknown, PageId::TimeOut, 0, "Timeout" });

	// A matching detection drives the best-match / detector-report trace lambdas.
	ScreenDataPage page(12);
	page[9].Text = "Equipment ON/OFF";
	BOOST_CHECK(model.DetectPage(page) == PageId::System);

	// No edges connect System to OneTouch -> FindPath exhausts the BFS and hits the
	// "no path found" trace lambda, returning an empty path.
	BOOST_CHECK(model.FindPath(PageId::System, PageId::OneTouch).empty());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Model Structural Integrity Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(ModelIntegrity_TestSuite)

BOOST_AUTO_TEST_CASE(TestSetTemperatureHasNoSelectEdges)
{
	// Verifies that SetPoolHeat/SetSpaHeat inline-edit pages were properly removed
	auto model = CreateOneTouchMenuModel();
	const MenuPage* temp_page = model.GetPage(PageId::SetTemperature);
	BOOST_REQUIRE(temp_page != nullptr);

	auto select_edges = GetSelectEdges(temp_page);
	BOOST_CHECK_EQUAL(select_edges.size(), 0);
}

BOOST_AUTO_TEST_CASE(TestChlorinatorPagesAreReachableFromMenu)
{
	// The Set AquaPure and Boost Pool pages must be reachable (via a Select edge from the
	// Menu/Help page) so the OneTouch can drive the chlorinator output % and boost. These
	// are conditional menu items (present only when a chlorinator is configured).
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	bool aquapure_reachable = false;
	bool boost_reachable = false;
	for (const auto& [id, page] : pages)
	{
		for (const auto& edge : page.edges)
		{
			if (edge.trigger == EdgeTrigger::Select && edge.IsPageTransition())
			{
				if (edge.target == PageId::SetAquapure) { aquapure_reachable = true; }
				if (edge.target == PageId::Boost) { boost_reachable = true; }
			}
		}
	}

	BOOST_CHECK_MESSAGE(aquapure_reachable, "SetAquapure has no incoming Select edge (chlorinator % unreachable)");
	BOOST_CHECK_MESSAGE(boost_reachable, "Boost has no incoming Select edge (chlorinator boost unreachable)");
}

BOOST_AUTO_TEST_CASE(TestCustomLabelHasNoIncomingEdges)
{
	// Verify no Select edge in the entire model targets CustomLabel
	auto model = CreateOneTouchMenuModel();
	const auto& pages = model.GetAllPages();

	for (const auto& [id, page] : pages)
	{
		for (const auto& edge : page.edges)
		{
			if (edge.trigger == EdgeTrigger::Select && edge.IsPageTransition())
			{
				BOOST_CHECK_MESSAGE(edge.target != PageId::CustomLabel,
					"Found Select edge to CustomLabel from page '" + page.name + "'");
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TestCustomLabelDetectorUsesInstructionText)
{
	// CustomLabel's title line (line 1) is user-entered content (variable).
	// Detector uses line 7 instruction text instead.
	auto model = CreateOneTouchMenuModel();
	const MenuPage* custom_label = model.GetPage(PageId::CustomLabel);
	BOOST_REQUIRE(custom_label != nullptr);
	BOOST_REQUIRE_EQUAL(custom_label->detectors.size(), 1);
	BOOST_CHECK_EQUAL(custom_label->detectors[0].line, 7);
	BOOST_CHECK_EQUAL(custom_label->detectors[0].pattern, "Use Arrow Keys");
}

BOOST_AUTO_TEST_CASE(TestLightLabelsDetectorVsEdgeLabelSpacing)
{
	// Detector uses page title "Light Labels" (1 space).
	// Edge label on LabelAux uses menu item text "Light   Labels" (3 spaces).
	// These are intentionally different due to column-aligned menu rendering.
	auto model = CreateOneTouchMenuModel();

	// Check detector
	const MenuPage* light_labels = model.GetPage(PageId::LightLabels);
	BOOST_REQUIRE(light_labels != nullptr);
	BOOST_REQUIRE_GE(light_labels->detectors.size(), 1);
	BOOST_CHECK_EQUAL(light_labels->detectors[0].pattern, "Light Labels");

	// Check edge label on LabelAux that targets LightLabels
	const MenuPage* label_aux = model.GetPage(PageId::LabelAux);
	BOOST_REQUIRE(label_aux != nullptr);

	bool found_light_edge = false;
	for (const auto& edge : label_aux->edges)
	{
		if (edge.target == PageId::LightLabels && edge.trigger == EdgeTrigger::Select)
		{
			BOOST_CHECK_EQUAL(edge.label, "Light   Labels");
			found_light_edge = true;
		}
	}
	BOOST_CHECK(found_light_edge);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Navigator Navigation Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(NavigatorNavigation_TestSuite)

// Helper: build page content with detector patterns and menu item text
static ScreenDataPage MakeSystemPage(const MenuModel& model)
{
	ScreenDataPage page(12);
	page[9].Text = "Equipment ON/OFF";
	page[10].Text = "OneTouch ON/OFF";
	page[11].Text = "Menu/Help";
	return page;
}

static ScreenDataPage MakeMenuHelpPage(const MenuModel& model)
{
	ScreenDataPage page(12);
	page[0].Text = "Menu";
	page[1].Text = "Help";
	page[2].Text = "Program";
	page[3].Text = "Set Temp";
	page[4].Text = "Set Time";
	page[6].Text = "Display Light";
	page[7].Text = "Lockouts";
	page[8].Text = "Password";
	page[9].Text = "Program Group";
	page[10].Text = "System Setup";
	return page;
}

// Helper: sync a navigator to a page via repeated consistent detections
static void SyncToPage(Navigator& nav, const ScreenDataPage& content)
{
	nav.StartSync();
	for (uint32_t i = 0; i < Navigator::SYNC_REQUIRED_CONSISTENT_COUNT; ++i)
	{
		nav.OnPageUpdate(content, 0);
	}
}

BOOST_AUTO_TEST_CASE(TestNavigateToSetsCorrectState)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	auto system_content = MakeSystemPage(model);
	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.IsSynced());
	BOOST_CHECK(nav.GetCurrentPage() == PageId::System);

	nav.NavigateTo(PageId::MenuHelp);
	BOOST_CHECK(nav.GetState() == Navigator::State::Navigating);
	BOOST_CHECK(nav.GetTargetPage() == PageId::MenuHelp);
}

BOOST_AUTO_TEST_CASE(TestNavigateToResetsAccumulatedState)
{
	// After NavigateTo(), accumulated recovery state should be reset so
	// a second navigation attempt starts fresh (no cascading failures).
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	auto system_content = MakeSystemPage(model);
	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.IsSynced());

	// Navigate to System (already there) — should arrive immediately
	nav.NavigateTo(PageId::System);
	auto cmd = nav.OnPageUpdate(system_content, 0);
	BOOST_CHECK(nav.GetState() == Navigator::State::AtDestination);

	// Start a second navigation — should work cleanly
	nav.NavigateTo(PageId::MenuHelp);
	BOOST_CHECK(nav.GetState() == Navigator::State::Navigating);

	// Provide System page with cursor at line 11 (Menu/Help)
	cmd = nav.OnPageUpdate(system_content, 11);
	// Should compute path and issue Select command
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(TestNavigationSelectGeneratesCorrectCommand)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	auto system_content = MakeSystemPage(model);
	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::System);

	// Navigate from System to MenuHelp — requires Select at line 11
	nav.NavigateTo(PageId::MenuHelp);

	// Feed page update with cursor already at line 11
	auto cmd = nav.OnPageUpdate(system_content, 11);

	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(TestNavigationBackGeneratesCorrectCommand)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	// Sync to MenuHelp
	auto menu_content = MakeMenuHelpPage(model);
	SyncToPage(nav, menu_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::MenuHelp);

	// Navigate from MenuHelp to System — shortest path uses Back edge
	nav.NavigateTo(PageId::System);

	auto cmd = nav.OnPageUpdate(menu_content, 0);

	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Back);
}

BOOST_AUTO_TEST_CASE(TestNavigationArrivalSetsAtDestination)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	auto system_content = MakeSystemPage(model);
	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::System);

	// Navigate from System to MenuHelp
	nav.NavigateTo(PageId::MenuHelp);

	// Step 1: Execute Select at line 11
	auto cmd = nav.OnPageUpdate(system_content, 11);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);

	// Simulate status messages completing the page transition
	nav.OnStatusMessageReceived();
	nav.OnStatusMessageReceived();

	// Step 2: Feed MenuHelp page content — should arrive at destination
	auto menu_content = MakeMenuHelpPage(model);
	cmd = nav.OnPageUpdate(menu_content, 0);

	BOOST_CHECK(nav.GetState() == Navigator::State::AtDestination);
	BOOST_CHECK(nav.GetCurrentPage() == PageId::MenuHelp);
}

BOOST_AUTO_TEST_CASE(TestHandleWaitingForPageChecksStateAfterRecompute)
{
	// When arriving at an unexpected but known page, the navigator should
	// recompute the path and continue navigating (not enter recovery).
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	auto system_content = MakeSystemPage(model);
	SyncToPage(nav, system_content);

	// Navigate to SystemSetup (System -> MenuHelp -> SystemSetup)
	nav.NavigateTo(PageId::SystemSetup);

	// Step 1: Execute first edge (Select at line 11 for Menu/Help)
	auto cmd = nav.OnPageUpdate(system_content, 11);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);
	nav.OnStatusMessageReceived();
	nav.OnStatusMessageReceived();

	// Step 2: Instead of MenuHelp, we arrive at System again (unexpected).
	// Navigator should recompute path from System and continue navigating.
	cmd = nav.OnPageUpdate(system_content, 11);

	// After recompute, state should be Navigating (not Reorienting/Failed),
	// and it should issue a new command (Select to go to MenuHelp again).
	BOOST_CHECK(nav.GetState() != Navigator::State::Failed);
	// It should either be navigating or have issued another Select
	BOOST_CHECK(nav.GetState() == Navigator::State::WaitingForPage ||
		nav.GetState() == Navigator::State::Navigating ||
		nav.GetState() == Navigator::State::MovingCursor);
}

// =============================================================================
// Content-Based Resolution (FindLineByLabel) Indirect Tests
// =============================================================================

BOOST_AUTO_TEST_CASE(TestContentBasedResolutionFindsCorrectLine)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	// Build a System page with "Menu/Help" at line 10 instead of 11
	ScreenDataPage system_content(12);
	system_content[9].Text = "Equipment ON/OFF";  // detector (unchanged)
	system_content[10].Text = "Menu/Help";          // shifted from line 11 to 10
	// line 11 is empty

	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::System);

	nav.NavigateTo(PageId::MenuHelp);

	// Cursor at line 10 (where "Menu/Help" actually is)
	// FindLineByLabel should resolve to line 10, and cursor matches
	auto cmd = nav.OnPageUpdate(system_content, 10);
	BOOST_REQUIRE(cmd.has_value());
	// Should issue Select directly (cursor already at resolved line 10)
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_CASE(TestContentBasedResolutionWordBoundary)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	// Build MenuHelp page content with "HelpChoose" at line 1 (word boundary)
	// and actual "Help >" at line 5
	ScreenDataPage menu_content(12);
	menu_content[0].Text = "Menu";           // detector
	menu_content[1].Text = "HelpChoose";      // NOT a match for "Help" (word boundary)
	menu_content[5].Text = "Help           >";  // Actual "Help" item

	SyncToPage(nav, menu_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::MenuHelp);

	nav.NavigateTo(PageId::HelpSubmenu);

	// Cursor at line 1. FindLineByLabel("Help") should skip "HelpChoose" (word boundary)
	// and find "Help >" at line 5. Cursor (1) != target (5), so cursor moves down.
	auto cmd = nav.OnPageUpdate(menu_content, 1);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::LineDown);
}

BOOST_AUTO_TEST_CASE(TestContentBasedResolutionWithLeadingSpaces)
{
	auto model = CreateOneTouchMenuModel();
	Navigator nav(model);

	// Build System page with leading/trailing whitespace on menu items
	ScreenDataPage system_content(12);
	system_content[9].Text = "  Equipment ON/OFF  ";
	system_content[10].Text = "  OneTouch ON/OFF  ";
	system_content[11].Text = "  Menu/Help           >";

	SyncToPage(nav, system_content);
	BOOST_REQUIRE(nav.GetCurrentPage() == PageId::System);

	nav.NavigateTo(PageId::MenuHelp);

	// Cursor at line 11 (where "  Menu/Help..." is).
	// FindLineByLabel trims leading/trailing whitespace and does starts_with.
	// Should resolve to line 11 and issue Select.
	auto cmd = nav.OnPageUpdate(system_content, 11);
	BOOST_REQUIRE(cmd.has_value());
	BOOST_CHECK(cmd.value() == NavKeyCommand::Select);
}

BOOST_AUTO_TEST_SUITE_END()
