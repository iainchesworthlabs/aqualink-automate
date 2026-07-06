#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "utility/screen_data_page.h"
#include "utility/screen_data_page_processor.h"

namespace AqualinkAutomate::Navigation
{

	// Forward declarations
	enum class PageId : uint32_t;

	// Detection pattern: line number + substring to match
	struct Detector
	{
		uint8_t line;
		std::string pattern;
	};

	// Trigger types for menu edges
	enum class EdgeTrigger : uint8_t
	{
		Select,         // Press Select — page transition (requires cursor on trigger_line)
		Back,           // Press Back — page transition to parent
		LineUp,         // Self-loop: move cursor up
		LineDown,       // Self-loop: move cursor down
		PageUp,         // Self-loop: scroll page up (scrollable pages only)
		PageDown,       // Self-loop: scroll page down
		SystemTimeout,  // Involuntary: controller timeout occurred
		SystemService   // Involuntary: controller entered service mode
	};

	// A directed edge in the menu graph
	struct MenuEdge
	{
		EdgeTrigger trigger;
		PageId source;          // from-node
		PageId target;          // to-node (== source for self-loops)
		uint8_t trigger_line;   // For Select: required highlighted line. 0 otherwise.
		std::string label;      // Human-readable (e.g. "Equipment ON/OFF")

		constexpr bool IsSelfLoop() const { return source == target; }
		constexpr bool IsPageTransition() const { return !IsSelfLoop(); }
	};

	// Describes a single page in the menu hierarchy
	struct MenuPage
	{
		PageId id{};
		std::string name;
		Utility::ScreenDataPageTypes page_type{};

		// Detection patterns - all must match to identify this page
		std::vector<Detector> detectors;

		// ALL edges originating from this page
		std::vector<MenuEdge> edges;

		// Optional: maximum number of non-empty lines for this page to match.
		// When set, detection requires the screen to have at most this many non-empty lines.
		std::optional<uint8_t> max_content_lines;

		// When true, the spider engine visits this page once per incoming Select edge
		// (rather than once total). Used for parameterized pages like LabelAux where
		// the same PageId shows different content depending on which menu item was selected.
		bool multi_instance = false;

		// When true, this page is a transient splash/cold-start screen the controller
		// auto-advances off on its own (no keypress). It has no navigation edges; the
		// navigator waits for it to clear rather than trying to navigate/recover from it.
		bool transient = false;

		// Convenience: find the Back edge target (nullopt if no Back edge)
		std::optional<PageId> BackTarget() const;

		// Convenience: check if this page supports a given trigger
		bool SupportsKey(EdgeTrigger trigger) const;
	};

	// Page identifiers for the OneTouch menu system
	enum class PageId : uint32_t
	{
		Unknown = 0,

		// Root pages
		System,             // Home/Equipment ON-OFF screen
		OneTouch,           // OneTouch ON/OFF screen
		MoreOneTouch,       // More OneTouch ON/OFF screen (scrolled)

		// Main menu pages
		MenuHelp,           // Main Menu/Help screen
		HelpSubmenu,        // Help submenu (Keys, Service, Diagnostics)

		// Help submenu pages
		HelpKeys,           // Help -> Keys
		HelpService,        // Help -> Service (version info)

		// Diagnostics pages
		DiagnosticsSensors,  // Model + Sensors
		DiagnosticsRemotes,  // Remotes
		DiagnosticsErrors,   // Errors
		DiagnosticsIAQStatus, // iAquaLink Status (connection info)
		DiagnosticsIAQRSSI,  // iAquaLink RSSI (signal strength)

		// System Setup pages
		SystemSetup,        // System Setup menu
		LabelAuxList,       // Label Aux list
		LabelAux,           // Individual label editing (multi-instance: visited per incoming edge)

		// Label Aux sub-pages
		GeneralLabels,      // General labels list
		LightLabels,        // Light labels list
		WaterfallLabels,    // Waterfall labels list
		CustomLabel,        // Custom label entry

		// Equipment pages
		EquipmentOnOff,     // Equipment ON/OFF control
		EquipmentStatus,    // Equipment status display

		// Settings pages
		SetTemperature,     // Temperature setting
		SetPoolHeat,        // Pool heat temperature adjustment
		SetSpaHeat,         // Spa heat temperature adjustment
		SetTime,            // Time setting
		FreezeProtect,      // Freeze protection
		Boost,              // Boost mode
		SetAquapure,        // Aquapure settings
		SelectSpeed,        // Speed selection

		// Menu/Help sub-pages (previously unimplemented)
		Program,            // Program schedule
		DisplayLight,       // Display light settings
		Lockouts,           // Lockout settings
		PasswordSettings,   // Password settings
		ProgramGroup,       // Program group settings

		// Special pages
		StartUp,            // Cold start splash screen (model/type/revision)
		Service,            // Service mode
		TimeOut,            // Timeout mode
		Version,            // Version display
		EnterPassword       // Password entry prompt (navigate back if encountered)
	};

	// The menu model containing all pages and navigation relationships
	class MenuModel
	{
	public:
		MenuModel() = default;

		// Register a page in the model
		void RegisterPage(MenuPage page);

		// Register a global edge (e.g. system timeout/service from any page)
		void RegisterGlobalEdge(MenuEdge edge);

		// Get a page by its ID (returns nullptr if not found)
		const MenuPage* GetPage(PageId id) const;

		// Find a page by matching detection patterns against screen content
		// Returns the best matching page, or nullptr if no match
		const MenuPage* FindPageByDetection(const Utility::ScreenDataPage& content) const;

		// Find the PageId by matching detection patterns
		PageId DetectPage(const Utility::ScreenDataPage& content) const;

		// Find the PageId by ScreenDataPageTypes (used to sync with page processors)
		PageId FindPageIdByType(Utility::ScreenDataPageTypes page_type) const;

		// Pathfinding: find the shortest path between two pages
		// Returns a sequence of edge pointers (valid as long as this MenuModel lives)
		// Returns empty vector if no path exists
		std::vector<const MenuEdge*> FindPath(PageId from, PageId to) const;

		// Find a global system event edge that matches a detected page
		std::optional<MenuEdge> FindSystemEvent(PageId detected_page) const;

		// Get all registered pages (for debugging/testing)
		const std::unordered_map<PageId, MenuPage>& GetAllPages() const { return m_Pages; }

		// Get all global edges (for debugging/testing)
		const std::vector<MenuEdge>& GetGlobalEdges() const { return m_GlobalEdges; }

		// Collect all incoming Select edges that transition to the given page
		std::vector<const MenuEdge*> GetIncomingSelectEdges(PageId target) const;

	private:
		// Rebuild the incoming-Select-edge index from the current page set. Cheap one-off
		// scan performed lazily on first query after a page registration.
		void RebuildIncomingSelectEdges() const;

		std::unordered_map<PageId, MenuPage> m_Pages;
		std::vector<MenuEdge> m_GlobalEdges;

		// Incoming-Select-edge index: target page -> edges that Select-transition into it.
		// Edge pointers are stable for the model's lifetime (page edge vectors are not
		// mutated after registration). Built lazily and memoised; invalidated by RegisterPage.
		// Marked mutable so the const query accessor can populate it on demand.
		mutable std::unordered_map<PageId, std::vector<const MenuEdge*>> m_IncomingSelectEdges;
		mutable bool m_IncomingSelectEdgesValid{ false };
	};

}
// namespace AqualinkAutomate::Navigation
