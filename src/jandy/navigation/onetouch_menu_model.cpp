#include "navigation/onetouch_menu_model.h"

#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Utility;

namespace AqualinkAutomate::Navigation
{

	MenuModel CreateOneTouchMenuModel()
	{
		MenuModel model;

		LogInfo(Channel::Navigation, "Creating OneTouch menu model");

		// =========================================================================
		// ROOT PAGES
		// =========================================================================

		// System/Home screen - the main landing page
		// Has 3 menu items at lines 9-11: Equipment ON/OFF, OneTouch ON/OFF, Menu/Help
		model.RegisterPage({
			.id = PageId::System,
			.name = "System",
			.page_type = ScreenDataPageTypes::Page_System,
			.detectors = {{ 9, "Equipment ON/OFF" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::System, PageId::EquipmentOnOff, 9,  "Equipment ON/OFF" },
				{ EdgeTrigger::Select,   PageId::System, PageId::OneTouch,       10, "OneTouch ON/OFF" },
				{ EdgeTrigger::Select,   PageId::System, PageId::MenuHelp,       11, "Menu/Help" },
				{ EdgeTrigger::LineUp,   PageId::System, PageId::System,         0,  "" },
				{ EdgeTrigger::LineDown, PageId::System, PageId::System,         0,  "" },
			}
		});

		// OneTouch ON/OFF screen
		// Only up/down/select work on this page - no Back edge
		// Select toggles equipment ON/OFF on most lines. "System" at line 11 navigates to System.
		// Note: "More OneTouch" is reached via scrolling (LineDown past bottom), not Select.
		// MoreOneTouch is structurally unreachable via navigation; it shares the same page but scrolled.
		model.RegisterPage({
			.id = PageId::OneTouch,
			.name = "OneTouch",
			.page_type = ScreenDataPageTypes::Page_OneTouch,
			.detectors = {{ 11, "System" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::OneTouch, PageId::System,       11, "System" },
				{ EdgeTrigger::LineUp,   PageId::OneTouch, PageId::OneTouch,     0,  "" },
				{ EdgeTrigger::LineDown, PageId::OneTouch, PageId::OneTouch,     0,  "" },
			}
		});

		// More OneTouch ON/OFF screen (scrolled from OneTouch page)
		model.RegisterPage({
			.id = PageId::MoreOneTouch,
			.name = "MoreOneTouch",
			.page_type = ScreenDataPageTypes::Page_MoreOneTouch,
			.detectors = {{ 10, "OneTouch ON/OFF" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::MoreOneTouch, PageId::OneTouch, 10, "OneTouch ON/OFF" },
				{ EdgeTrigger::Select,   PageId::MoreOneTouch, PageId::System,   11, "System" },
				{ EdgeTrigger::LineUp,   PageId::MoreOneTouch, PageId::MoreOneTouch, 0, "" },
				{ EdgeTrigger::LineDown, PageId::MoreOneTouch, PageId::MoreOneTouch, 0, "" },
			}
		});

		// =========================================================================
		// MAIN MENU
		// =========================================================================

		// Menu/Help screen - main menu with many items
		model.RegisterPage({
			.id = PageId::MenuHelp,
			.name = "Menu",
			.page_type = ScreenDataPageTypes::Page_MenuHelp,
			.detectors = {{ 0, "Menu" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::HelpSubmenu,      1,  "Help" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::Program,          2,  "Program" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::SetTemperature,   3,  "Set Temp" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::SetTime,          4,  "Set Time" },
				// Note: the SetAquapure + Boost menu items are conditional (require a chlorinator).
				// When absent, items below shift up; content-based resolution handles shifted
				// positions, so the trigger_line is only a hint and the label is matched on screen.
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::SetAquapure,      5,  "Set AquaPure" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::DisplayLight,     6,  "Display Light" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::Lockouts,         7,  "Lockouts" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::PasswordSettings, 8,  "Password" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::ProgramGroup,     9,  "Program Group" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::SystemSetup,      10, "System Setup" },
				{ EdgeTrigger::Select,   PageId::MenuHelp, PageId::Boost,            11, "Boost Pool" },
				{ EdgeTrigger::Back,     PageId::MenuHelp, PageId::System,           0,  "" },
				{ EdgeTrigger::LineUp,   PageId::MenuHelp, PageId::MenuHelp,         0,  "" },
				{ EdgeTrigger::LineDown, PageId::MenuHelp, PageId::MenuHelp,         0,  "" },
			}
		});

		// =========================================================================
		// HELP SUBMENU
		// =========================================================================

		// Help submenu (Keys, Service, Diagnostics, About)
		// Screen layout: line 0 = "Help" title, lines 1-3 = instruction text,
		// line 5 = "Go Back", lines 7-10 = Keys/Service/Diagnostics/About
		model.RegisterPage({
			.id = PageId::HelpSubmenu,
			.name = "Help",
			.page_type = ScreenDataPageTypes::Page_HelpSubmenu,
			.detectors = {{ 0, "Help" }, { 7, "Keys" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::HelpSubmenu, PageId::HelpKeys,           7, "Keys" },
				{ EdgeTrigger::Select,   PageId::HelpSubmenu, PageId::HelpService,        8, "Service" },
				{ EdgeTrigger::Select,   PageId::HelpSubmenu, PageId::DiagnosticsSensors, 9, "Diagnostics" },
				{ EdgeTrigger::Back,     PageId::HelpSubmenu, PageId::MenuHelp,           0, "" },
				{ EdgeTrigger::LineUp,   PageId::HelpSubmenu, PageId::HelpSubmenu,        0, "" },
				{ EdgeTrigger::LineDown, PageId::HelpSubmenu, PageId::HelpSubmenu,        0, "" },
			}
		});

		// Help -> Keys page (shows key button descriptions)
		// Actual screen: line 0 = "    Key Help    ", line 11 = "    Continue    "
		model.RegisterPage({
			.id = PageId::HelpKeys,
			.name = "HelpKeys",
			.page_type = ScreenDataPageTypes::Page_HelpKeys,
			.detectors = {{ 0, "Key Help" }, { 11, "Continue" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::HelpKeys, PageId::HelpSubmenu, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::HelpKeys, PageId::HelpKeys,   0, "" },
				{ EdgeTrigger::LineDown, PageId::HelpKeys, PageId::HelpKeys,   0, "" },
			}
		});

		// Help -> Service page (service contact info)
		// Actual screen: line 0 = "  Service Help  ", line 11 = "    Continue    "
		model.RegisterPage({
			.id = PageId::HelpService,
			.name = "HelpService",
			.page_type = ScreenDataPageTypes::Page_Version,
			.detectors = {{ 0, "Service Help" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::HelpService, PageId::HelpSubmenu, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::HelpService, PageId::HelpService, 0, "" },
				{ EdgeTrigger::LineDown, PageId::HelpService, PageId::HelpService, 0, "" },
			}
		});

		// =========================================================================
		// DIAGNOSTICS PAGES
		// =========================================================================

		// Diagnostics - Model + Sensors
		// These pages cycle: Select moves SENSORS -> REMOTES -> ERRORS -> back to Help
		model.RegisterPage({
			.id = PageId::DiagnosticsSensors,
			.name = "DiagnosticsSensors",
			.page_type = ScreenDataPageTypes::Page_DiagnosticsSensors,
			.detectors = {{ 6, "Sensors" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::DiagnosticsSensors, PageId::DiagnosticsRemotes, 0, "Next" },
				{ EdgeTrigger::Back,     PageId::DiagnosticsSensors, PageId::HelpSubmenu,        0, "" },
				{ EdgeTrigger::LineUp,   PageId::DiagnosticsSensors, PageId::DiagnosticsSensors,  0, "" },
				{ EdgeTrigger::LineDown, PageId::DiagnosticsSensors, PageId::DiagnosticsSensors,  0, "" },
			}
		});

		// Diagnostics - Remotes
		model.RegisterPage({
			.id = PageId::DiagnosticsRemotes,
			.name = "DiagnosticsRemotes",
			.page_type = ScreenDataPageTypes::Page_DiagnosticsRemotes,
			.detectors = {{ 0, "Remotes" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::DiagnosticsRemotes, PageId::DiagnosticsErrors,  0, "Next" },
				{ EdgeTrigger::Back,     PageId::DiagnosticsRemotes, PageId::DiagnosticsSensors,  0, "" },
				{ EdgeTrigger::LineUp,   PageId::DiagnosticsRemotes, PageId::DiagnosticsRemotes,  0, "" },
				{ EdgeTrigger::LineDown, PageId::DiagnosticsRemotes, PageId::DiagnosticsRemotes,  0, "" },
			}
		});

		// Diagnostics - Errors
		// Select/Continue advances to iAquaLink Status (not directly back to Help)
		model.RegisterPage({
			.id = PageId::DiagnosticsErrors,
			.name = "DiagnosticsErrors",
			.page_type = ScreenDataPageTypes::Page_DiagnosticsErrors,
			.detectors = {{ 0, "Errors" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::DiagnosticsErrors, PageId::DiagnosticsIAQStatus, 0, "Continue" },
				{ EdgeTrigger::Back,     PageId::DiagnosticsErrors, PageId::DiagnosticsRemotes,   0, "" },
				{ EdgeTrigger::LineUp,   PageId::DiagnosticsErrors, PageId::DiagnosticsErrors,    0, "" },
				{ EdgeTrigger::LineDown, PageId::DiagnosticsErrors, PageId::DiagnosticsErrors,    0, "" },
			}
		});

		// Diagnostics - iAquaLink Status (connection info)
		// Actual screen: line 0 = "iAquaLink Status", line 11 = "    Continue    "
		model.RegisterPage({
			.id = PageId::DiagnosticsIAQStatus,
			.name = "DiagnosticsIAQStatus",
			.page_type = ScreenDataPageTypes::Page_DiagnosticsIAQStatus,
			.detectors = {{ 0, "iAquaLink Status" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::DiagnosticsIAQStatus, PageId::DiagnosticsIAQRSSI, 0, "Continue" },
				{ EdgeTrigger::Back,     PageId::DiagnosticsIAQStatus, PageId::DiagnosticsErrors,  0, "" },
				{ EdgeTrigger::LineUp,   PageId::DiagnosticsIAQStatus, PageId::DiagnosticsIAQStatus, 0, "" },
				{ EdgeTrigger::LineDown, PageId::DiagnosticsIAQStatus, PageId::DiagnosticsIAQStatus, 0, "" },
			}
		});

		// Diagnostics - iAquaLink RSSI (signal strength)
		// Actual screen: line 0 = "iAquaLink RSSI", line 11 = "    Continue    "
		model.RegisterPage({
			.id = PageId::DiagnosticsIAQRSSI,
			.name = "DiagnosticsIAQRSSI",
			.page_type = ScreenDataPageTypes::Page_DiagnosticsIAQRSSI,
			.detectors = {{ 0, "iAquaLink RSSI" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::DiagnosticsIAQRSSI, PageId::HelpSubmenu,         0, "Continue" },
				{ EdgeTrigger::Back,     PageId::DiagnosticsIAQRSSI, PageId::DiagnosticsIAQStatus, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::DiagnosticsIAQRSSI, PageId::DiagnosticsIAQRSSI,   0, "" },
				{ EdgeTrigger::LineDown, PageId::DiagnosticsIAQRSSI, PageId::DiagnosticsIAQRSSI,   0, "" },
			}
		});

		// =========================================================================
		// SYSTEM SETUP
		// =========================================================================

		// System Setup menu
		model.RegisterPage({
			.id = PageId::SystemSetup,
			.name = "SystemSetup",
			.page_type = ScreenDataPageTypes::Page_SystemSetup,
			.detectors = {{ 0, "System Setup" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::SystemSetup, PageId::LabelAuxList, 2, "Label Aux" },
				{ EdgeTrigger::Back,     PageId::SystemSetup, PageId::MenuHelp,     0, "" },
				{ EdgeTrigger::LineUp,   PageId::SystemSetup, PageId::SystemSetup,  0, "" },
				{ EdgeTrigger::LineDown, PageId::SystemSetup, PageId::SystemSetup,  0, "" },
			}
		});

		// Label Aux List (scrollable - "^^ More vv" indicator)
		//
		// Dynamic-scrolling aux discovery:
		//   The list shows one Select edge per installable aux output.  Power
		//   center A's aux outputs (Aux1-Aux7) fit on the first screen, but the
		//   B/C/D power centers (Aux B1-D8) appear AFTER them and only become
		//   visible by scrolling the list.  Because each aux is a separate
		//   incoming Select edge to the multi-instance LabelAux page, the
		//   SpiderEngine visits every one in turn; for each, the Navigator
		//   resolves the row by its LABEL via FindLineByLabel and, when the row
		//   is below the fold, scrolls (PageDown) until it appears
		//   (NavigateToItem / MAX_ITEM_SCROLL_ATTEMPTS).  This is what makes the
		//   B/C/D auxes discoverable without fixed line positions.
		//
		//   The label here is the controller's DEFAULT row text (e.g. "Aux B1").
		//   The "Label Aux" list always renders these default Aux IDs (the
		//   user-assigned custom name is shown on the LabelAux detail page, not
		//   in this list), so a case-insensitive prefix match by FindLineByLabel
		//   reliably locates each row regardless of any custom naming.
		//
		//   trigger_line is a starting cursor hint AND the per-edge identity key
		//   used by the SpiderEngine's multi-instance visited set
		//   ({source, trigger_line}); it must therefore be UNIQUE per edge.  The
		//   B/C/D rows are off-screen until scrolled, so their hint values are
		//   synthetic monotonic indices (>= the screen height) - the actual line
		//   is always resolved from on-screen content at run time.
		//
		//   Controllers without a given power center simply never render those
		//   rows; the Navigator fails to find them after scrolling and the
		//   SpiderEngine treats that as a benign per-edge skip (NOT a crawl
		//   failure) and moves on to the next aux.
		model.RegisterPage({
			.id = PageId::LabelAuxList,
			.name = "LabelAuxList",
			.page_type = ScreenDataPageTypes::Page_LabelAuxList,
			.detectors = {{ 0, "Label Aux" }},
			.edges = {
				// Power center A aux devices (always present, fixed lines 2-8).
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     2, "Aux1" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     3, "Aux2" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     4, "Aux3" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     5, "Aux4" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     6, "Aux5" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     7, "Aux6" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     8, "Aux7" },
				// Power center B aux devices (discovered by scrolling). Synthetic,
				// unique trigger_line hints; real position resolved from content.
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     9,  "Aux B1" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     10, "Aux B2" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     11, "Aux B3" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     12, "Aux B4" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     13, "Aux B5" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     14, "Aux B6" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     15, "Aux B7" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     16, "Aux B8" },
				// Power center C aux devices (discovered by scrolling).
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     17, "Aux C1" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     18, "Aux C2" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     19, "Aux C3" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     20, "Aux C4" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     21, "Aux C5" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     22, "Aux C6" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     23, "Aux C7" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     24, "Aux C8" },
				// Power center D aux devices (discovered by scrolling).
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     25, "Aux D1" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     26, "Aux D2" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     27, "Aux D3" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     28, "Aux D4" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     29, "Aux D5" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     30, "Aux D6" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     31, "Aux D7" },
				{ EdgeTrigger::Select,   PageId::LabelAuxList, PageId::LabelAux,     32, "Aux D8" },
				// Navigation
				{ EdgeTrigger::Back,     PageId::LabelAuxList, PageId::SystemSetup,  0, "" },
				{ EdgeTrigger::LineUp,   PageId::LabelAuxList, PageId::LabelAuxList, 0, "" },
				{ EdgeTrigger::LineDown, PageId::LabelAuxList, PageId::LabelAuxList, 0, "" },
				{ EdgeTrigger::PageUp,   PageId::LabelAuxList, PageId::LabelAuxList, 0, "" },
				{ EdgeTrigger::PageDown, PageId::LabelAuxList, PageId::LabelAuxList, 0, "" },
			}
		});

		// Label Aux detail page (individual label editing)
		// multi_instance: the spider visits this page once per incoming Select edge from
		// LabelAuxList, since each edge shows a different aux device's current label.
		model.RegisterPage({
			.id = PageId::LabelAux,
			.name = "LabelAux",
			.page_type = ScreenDataPageTypes::Page_LabelAux,
			.detectors = {{ 2, "Current Label" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::LabelAux, PageId::GeneralLabels,   5, "General Labels" },
				{ EdgeTrigger::Select,   PageId::LabelAux, PageId::LightLabels,     6, "Light   Labels" },
				{ EdgeTrigger::Select,   PageId::LabelAux, PageId::WaterfallLabels, 7, "Waterfall Labels" },
				// Note: CustomLabel (line 8) is an interactive character editor, not a navigation target.
				// Navigating into it would modify the label. Excluded from crawl.
				{ EdgeTrigger::Back,     PageId::LabelAux, PageId::LabelAuxList,    0, "" },
				{ EdgeTrigger::LineUp,   PageId::LabelAux, PageId::LabelAux,        0, "" },
				{ EdgeTrigger::LineDown, PageId::LabelAux, PageId::LabelAux,        0, "" },
			},
			.multi_instance = true,
		});

		// =========================================================================
		// LABEL AUX SUB-PAGES
		// =========================================================================

		// General labels list
		// Actual screen: line 0 = "   Label AuxN   " (parent header), line 1 = " General Labels"
		// Back goes to LabelAux detail page (detection may briefly show LabelAuxList during transition)
		model.RegisterPage({
			.id = PageId::GeneralLabels,
			.name = "GeneralLabels",
			.page_type = ScreenDataPageTypes::Page_GeneralLabels,
			.detectors = {{ 1, "General Labels" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::GeneralLabels, PageId::LabelAux,  0, "" },
				{ EdgeTrigger::LineUp,   PageId::GeneralLabels, PageId::GeneralLabels,  0, "" },
				{ EdgeTrigger::LineDown, PageId::GeneralLabels, PageId::GeneralLabels,  0, "" },
			}
		});

		// Light labels list
		// Actual screen: line 0 = "   Label AuxN   " (parent header), line 1 = "  Light Labels  " (page title)
		// Note: menu item on LabelAux is "Light   Labels >" (3-space gap, column-aligned),
		// but the page title itself uses standard 1-space. Detector matches page title.
		model.RegisterPage({
			.id = PageId::LightLabels,
			.name = "LightLabels",
			.page_type = ScreenDataPageTypes::Page_LightLabels,
			.detectors = {{ 1, "Light Labels" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::LightLabels, PageId::LabelAux,  0, "" },
				{ EdgeTrigger::LineUp,   PageId::LightLabels, PageId::LightLabels,    0, "" },
				{ EdgeTrigger::LineDown, PageId::LightLabels, PageId::LightLabels,    0, "" },
			}
		});

		// Waterfall labels list
		// Actual screen: line 0 = "   Label AuxN   " (parent header), line 1 = title
		model.RegisterPage({
			.id = PageId::WaterfallLabels,
			.name = "WaterfallLabels",
			.page_type = ScreenDataPageTypes::Page_WaterfallLabels,
			.detectors = {{ 1, "Wtrfall Labels" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::WaterfallLabels, PageId::LabelAux,    0, "" },
				{ EdgeTrigger::LineUp,   PageId::WaterfallLabels, PageId::WaterfallLabels,  0, "" },
				{ EdgeTrigger::LineDown, PageId::WaterfallLabels, PageId::WaterfallLabels,  0, "" },
			}
		});

		// Custom label entry page — interactive character editor
		// Actual screen: line 0 = "   Label AuxN   ", line 1 = current custom name (variable),
		// lines 7-11 = editing instructions ("Use Arrow Keys to change letter...")
		// Detector uses instruction text since the title (line 1) is user-entered content.
		// This page is NOT a navigation target (no incoming edges) to avoid accidental edits.
		model.RegisterPage({
			.id = PageId::CustomLabel,
			.name = "CustomLabel",
			.page_type = ScreenDataPageTypes::Page_CustomLabel,
			.detectors = {{ 7, "Use Arrow Keys" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::CustomLabel, PageId::LabelAux,  0, "" },
			}
		});

		// =========================================================================
		// EQUIPMENT PAGES
		// =========================================================================

		// Equipment ON/OFF page (scrollable - "^^ More vv" indicator)
		// Only up/down/select/page work - no Back edge (OneTouch-style page)
		model.RegisterPage({
			.id = PageId::EquipmentOnOff,
			.name = "EquipmentOnOff",
			.page_type = ScreenDataPageTypes::Page_EquipmentOnOff,
			.detectors = {{ 0, "Filter Pump" }},
			.edges = {
				{ EdgeTrigger::Select,   PageId::EquipmentOnOff, PageId::System,         11, "System" },
				{ EdgeTrigger::LineUp,   PageId::EquipmentOnOff, PageId::EquipmentOnOff,  0, "" },
				{ EdgeTrigger::LineDown, PageId::EquipmentOnOff, PageId::EquipmentOnOff,  0, "" },
				{ EdgeTrigger::PageUp,   PageId::EquipmentOnOff, PageId::EquipmentOnOff,  0, "" },
				{ EdgeTrigger::PageDown, PageId::EquipmentOnOff, PageId::EquipmentOnOff,  0, "" },
			}
		});

		// Equipment Status page
		model.RegisterPage({
			.id = PageId::EquipmentStatus,
			.name = "EquipmentStatus",
			.page_type = ScreenDataPageTypes::Page_EquipmentStatus,
			.detectors = {{ 0, "EQUIPMENT STATUS" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::EquipmentStatus, PageId::System,          0, "" },
				{ EdgeTrigger::LineUp,   PageId::EquipmentStatus, PageId::EquipmentStatus, 0, "" },
				{ EdgeTrigger::LineDown, PageId::EquipmentStatus, PageId::EquipmentStatus, 0, "" },
			}
		});

		// =========================================================================
		// SETTINGS PAGES
		// =========================================================================

		// Set Temperature page
		// Note: Pool Heat/Spa Heat are inline edits on this page, not separate page transitions.
		// Pressing Select on those lines adjusts temperature in-place rather than navigating away.
		model.RegisterPage({
			.id = PageId::SetTemperature,
			.name = "SetTemperature",
			.page_type = ScreenDataPageTypes::Page_SetTemperature,
			.detectors = {{ 0, "Set Temp" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::SetTemperature, PageId::MenuHelp,       0, "" },
				{ EdgeTrigger::LineUp,   PageId::SetTemperature, PageId::SetTemperature,  0, "" },
				{ EdgeTrigger::LineDown, PageId::SetTemperature, PageId::SetTemperature,  0, "" },
			}
		});

		// Note: SetPoolHeat and SetSpaHeat are not separate pages. They are inline
		// temperature edits on the SetTemperature page. Page registrations removed.

		// Set Time page
		model.RegisterPage({
			.id = PageId::SetTime,
			.name = "SetTime",
			.page_type = ScreenDataPageTypes::Page_SetTime,
			.detectors = {{ 0, "Set Time" }, { 7, "Use Arrow Keys" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::SetTime, PageId::MenuHelp, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::SetTime, PageId::SetTime,  0, "" },
				{ EdgeTrigger::LineDown, PageId::SetTime, PageId::SetTime,  0, "" },
			}
		});

		// Freeze Protect page
		model.RegisterPage({
			.id = PageId::FreezeProtect,
			.name = "FreezeProtect",
			.page_type = ScreenDataPageTypes::Page_FreezeProtect,
			.detectors = {{ 0, "Freeze Protect" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::FreezeProtect, PageId::MenuHelp,     0, "" },
				{ EdgeTrigger::LineUp,   PageId::FreezeProtect, PageId::FreezeProtect, 0, "" },
				{ EdgeTrigger::LineDown, PageId::FreezeProtect, PageId::FreezeProtect, 0, "" },
			}
		});

		// Boost page
		model.RegisterPage({
			.id = PageId::Boost,
			.name = "Boost",
			.page_type = ScreenDataPageTypes::Page_Boost,
			.detectors = {{ 0, "Boost Pool" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::Boost, PageId::MenuHelp, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::Boost, PageId::Boost,    0, "" },
				{ EdgeTrigger::LineDown, PageId::Boost, PageId::Boost,    0, "" },
			}
		});

		// Set Aquapure page
		model.RegisterPage({
			.id = PageId::SetAquapure,
			.name = "SetAquapure",
			.page_type = ScreenDataPageTypes::Page_SetAquapure,
			.detectors = {{ 0, "Set AquaPure" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::SetAquapure, PageId::MenuHelp,    0, "" },
				{ EdgeTrigger::LineUp,   PageId::SetAquapure, PageId::SetAquapure, 0, "" },
				{ EdgeTrigger::LineDown, PageId::SetAquapure, PageId::SetAquapure, 0, "" },
			}
		});

		// Select Speed page
		model.RegisterPage({
			.id = PageId::SelectSpeed,
			.name = "SelectSpeed",
			.page_type = ScreenDataPageTypes::Page_SelectSpeed,
			.detectors = {{ 0, "Select Speed" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::SelectSpeed, PageId::EquipmentOnOff, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::SelectSpeed, PageId::SelectSpeed,    0, "" },
				{ EdgeTrigger::LineDown, PageId::SelectSpeed, PageId::SelectSpeed,    0, "" },
			}
		});

		// =========================================================================
		// MENU/HELP SUB-PAGES (previously unimplemented)
		// =========================================================================

		// Program schedule page
		model.RegisterPage({
			.id = PageId::Program,
			.name = "Program",
			.page_type = ScreenDataPageTypes::Page_Program,
			.detectors = {{ 0, "Program" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::Program, PageId::MenuHelp, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::Program, PageId::Program,  0, "" },
				{ EdgeTrigger::LineDown, PageId::Program, PageId::Program,  0, "" },
			}
		});

		// Display light settings page
		model.RegisterPage({
			.id = PageId::DisplayLight,
			.name = "DisplayLight",
			.page_type = ScreenDataPageTypes::Page_DisplayLight,
			.detectors = {{ 0, "Display Light" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::DisplayLight, PageId::MenuHelp,     0, "" },
				{ EdgeTrigger::LineUp,   PageId::DisplayLight, PageId::DisplayLight,  0, "" },
				{ EdgeTrigger::LineDown, PageId::DisplayLight, PageId::DisplayLight,  0, "" },
			}
		});

		// Lockout settings page
		model.RegisterPage({
			.id = PageId::Lockouts,
			.name = "Lockouts",
			.page_type = ScreenDataPageTypes::Page_Lockouts,
			.detectors = {{ 0, "Lockout" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::Lockouts, PageId::MenuHelp, 0, "" },
				{ EdgeTrigger::LineUp,   PageId::Lockouts, PageId::Lockouts,  0, "" },
				{ EdgeTrigger::LineDown, PageId::Lockouts, PageId::Lockouts,  0, "" },
			}
		});

		// Password settings page
		model.RegisterPage({
			.id = PageId::PasswordSettings,
			.name = "PasswordSettings",
			.page_type = ScreenDataPageTypes::Page_PasswordSettings,
			.detectors = {{ 0, "Password" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::PasswordSettings, PageId::MenuHelp,         0, "" },
				{ EdgeTrigger::LineUp,   PageId::PasswordSettings, PageId::PasswordSettings,  0, "" },
				{ EdgeTrigger::LineDown, PageId::PasswordSettings, PageId::PasswordSettings,  0, "" },
			}
		});

		// Program group settings page
		model.RegisterPage({
			.id = PageId::ProgramGroup,
			.name = "ProgramGroup",
			.page_type = ScreenDataPageTypes::Page_ProgramGroup,
			.detectors = {{ 0, "Program Group" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::ProgramGroup, PageId::MenuHelp,     0, "" },
				{ EdgeTrigger::LineUp,   PageId::ProgramGroup, PageId::ProgramGroup,  0, "" },
				{ EdgeTrigger::LineDown, PageId::ProgramGroup, PageId::ProgramGroup,  0, "" },
			}
		});

		// =========================================================================
		// SPECIAL PAGES
		// =========================================================================

		// StartUp splash screen (cold start) - shows model number, type, and revision
		// Has 2 detectors to be more specific than HelpService (which only checks line 7)
		// max_content_lines = 4 provides structural confirmation (3 data lines + margin)
		// vs HelpService/Version page which has many more populated lines
		model.RegisterPage({
			.id = PageId::StartUp,
			.name = "StartUp",
			.page_type = ScreenDataPageTypes::Page_StartUp,
			.detectors = {{ 7, "REV " }, { 5, "-" }},
			.edges = {},  // Transient page - controller auto-transitions to a default page
			.max_content_lines = 4,
			.transient = true   // controller auto-advances off the splash; navigator waits rather than failing
		});

		// Service Mode page
		model.RegisterPage({
			.id = PageId::Service,
			.name = "Service",
			.page_type = ScreenDataPageTypes::Page_Service,
			.detectors = {{ 3, "Service Mode" }},
			.edges = {}  // No navigation possible from service mode
		});

		// Timeout Mode page
		model.RegisterPage({
			.id = PageId::TimeOut,
			.name = "TimeOut",
			.page_type = ScreenDataPageTypes::Page_TimeOut,
			.detectors = {{ 3, "Timeout Mode" }},
			.edges = {}  // No navigation possible from timeout mode
		});

		// Enter Password page - appears when selecting password-protected options
		model.RegisterPage({
			.id = PageId::EnterPassword,
			.name = "EnterPassword",
			.page_type = ScreenDataPageTypes::Page_EnterPassword,
			.detectors = {{ 0, "Enter Password" }},
			.edges = {
				{ EdgeTrigger::Back,     PageId::EnterPassword, PageId::MenuHelp,      0, "" },
				{ EdgeTrigger::LineUp,   PageId::EnterPassword, PageId::EnterPassword,  0, "" },
				{ EdgeTrigger::LineDown, PageId::EnterPassword, PageId::EnterPassword,  0, "" },
			}
		});

		// =========================================================================
		// GLOBAL SYSTEM EVENT EDGES
		// =========================================================================

		model.RegisterGlobalEdge({ EdgeTrigger::SystemTimeout, PageId::Unknown, PageId::TimeOut, 0, "Timeout" });
		model.RegisterGlobalEdge({ EdgeTrigger::SystemService, PageId::Unknown, PageId::Service, 0, "Service Mode" });

		LogInfo(Channel::Navigation, std::format("OneTouch menu model created with {} pages",
			model.GetAllPages().size()));

		return model;
	}

	std::optional<std::string> OneTouchPageCapabilityRequirement(PageId page)
	{
		using enum PageId;

		switch (page)
		{
		// iAqualink connection-status / signal pages only exist on panels with an iAqualink
		// (AqualinkTouch) interface fitted. Absent on the common OneTouch-only / RS panels.
		case DiagnosticsIAQStatus:
		case DiagnosticsIAQRSSI:
			return "requires iAqualink";

		// Boost and the Aquapure setpoint page only exist when a salt water chlorinator (SWG)
		// is installed; a panel without one never shows them.
		case Boost:
		case SetAquapure:
			return "requires a salt chlorinator";

		default:
			return std::nullopt;
		}
	}

}
// namespace AqualinkAutomate::Navigation
