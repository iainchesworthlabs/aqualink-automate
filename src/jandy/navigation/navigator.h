#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "navigation/menu_model.h"
#include "utility/screen_data_page.h"
#include "utility/screen_data_page_processor.h"

namespace AqualinkAutomate::Navigation
{

	// Key commands that can be sent to the device
	// These mirror the OneTouch KeyCommands but are defined here for independence
	enum class NavKeyCommand : uint8_t
	{
		NoCommand = 0x00,
		PageDown_Or_Select1 = 0x01,
		Back = 0x02,
		PageUp_Or_Select3 = 0x03,
		Select = 0x04,
		LineDown = 0x05,
		LineUp = 0x06
	};

	class Navigator
	{
	public:
		enum class State
		{
			Idle,               // No active navigation
			Syncing,            // Determining current page via consecutive detections
			Navigating,         // Executing path steps
			WaitingForPage,     // Sent command, waiting for page update
			MovingCursor,       // Moving cursor to target line
			EnteringPassword,   // Entering password digits
			Reorienting,        // Lost position, trying to recover
			AtDestination,      // Reached target page
			Failed              // Unrecoverable error
		};

		static constexpr uint32_t MAX_RECOVERY_ATTEMPTS = 3;
		static constexpr uint32_t MAX_BACK_PRESSES = 10;
		static constexpr uint32_t MAX_RECOMPUTE_COUNT = 50;      // Prevent infinite recompute loops
		static constexpr uint32_t MAX_STUCK_RECOMPUTES = 3;      // Fail a target after this many recomputes landing on the SAME page (target unreachable on this model, e.g. an IAQ-only Diagnostics page on a non-iAqualink panel)
		static constexpr uint32_t MAX_WAIT_CYCLES = 100;         // Timeout after this many page updates with no progress
		static constexpr uint32_t MAX_TRANSIENT_WAITS = 20;      // Page updates to wait for a transient (splash) page to auto-clear
		static constexpr uint32_t MAX_CURSOR_MOVES = 15;         // Max cursor moves before declaring wrap
		static constexpr uint32_t SYNC_REQUIRED_CONSISTENT_COUNT = 3; // Consecutive consistent detections needed for sync

		// Sentinel highlighted-line value meaning "no line is highlighted". The device sends a
		// clear-all highlight (0xFF) to drop the cursor; treating that literal value as a real
		// row index would leave the navigator chasing a non-existent line. When this arrives we
		// retain the previously known cursor line rather than overwriting it with the sentinel.
		static constexpr uint8_t CURSOR_LINE_NONE = 0xFF;

	public:
		explicit Navigator(const MenuModel& model);

		// Set password for menu navigation (4-digit numeric)
		void SetPassword(const std::string& password) { m_Password = password; }

		// Begin startup synchronization: detect current page via repeated detection
		void StartSync();

		// Start navigation to a specific page
		void NavigateTo(PageId target);

		// Start navigation to a specific menu item on a page.
		// When item_label is provided, the navigator uses content-based matching to find the
		// item on screen and scrolls the page if needed (for scrollable lists like LabelAuxList).
		void NavigateToItem(PageId page, uint8_t menu_item_line, const std::string& item_label = "", PageId select_target = PageId::Unknown);

		// Called when Status message is received to decrement wait counter
		void OnStatusMessageReceived();

		// Called on each screen update with current page info
		// Returns the next command to send (or nullopt if waiting/done)
		std::optional<NavKeyCommand> OnPageUpdate(
			const Utility::ScreenDataPage& content,
			uint8_t highlighted_line);

		// Get current state
		State GetState() const { return m_State; }

		// Get current detected page
		PageId GetCurrentPage() const { return m_CurrentPage; }

		// Get target page
		PageId GetTargetPage() const { return m_TargetPage; }

		// Get the current cursor/highlight line
		uint8_t GetCursorLine() const { return m_CursorLine; }

		// Check if sync is complete (navigator knows its position)
		bool IsSynced() const { return m_CurrentPage != PageId::Unknown; }

		// Check if navigation is complete (success or failure)
		bool IsComplete() const;

		// Check if navigation succeeded
		bool IsSuccess() const { return m_State == State::AtDestination; }

		// Reset navigator to idle state
		void Reset();

	private:
		// Handle the Syncing state: apply the consistency requirement to the already-resolved
		// detected page id (DetectPage is run once per content update by OnPageUpdate).
		std::optional<NavKeyCommand> HandleSyncing(PageId detected);

		// Compute navigation path from current to target
		void ComputePath();

		// Execute the next step in the path
		std::optional<NavKeyCommand> ExecuteNextStep();

		// Handle waiting for page state
		std::optional<NavKeyCommand> HandleWaitingForPage();

		// Handle arrival at an unexpected page
		void HandleUnexpectedPage(PageId actual);

		// Sub-steps of HandleUnexpectedPage extracted for clarity. HandleTransientPage waits
		// for a self-clearing splash page (returns true when it has taken over handling and the
		// caller should return); DetectStuckRecompute fails fast when the same target is
		// repeatedly unreachable (returns true when it has entered the Failed state).
		bool HandleTransientPage(PageId actual, const MenuPage* actual_page);
		bool DetectStuckRecompute(PageId actual, const MenuPage* target_page);

		// Start recovery process (back to home)
		void InitiateRecovery();

		// Execute next recovery step
		std::optional<NavKeyCommand> RecoveryNext();

		// Move cursor to the target line for the current step
		std::optional<NavKeyCommand> MoveCursorToTarget();

		// Wrap-recovery helpers for MoveCursorToTarget. AttemptWrapRecovery tries to retarget
		// the cursor via content-based label resolution after a wrap is detected; it returns
		// true (setting 'recovered_at_target' when the cursor already sits on the new target)
		// if a fresh target was found. GiveUpCursorMove abandons the move, either failing the
		// navigation (NavigateToItem) or accepting the current line as the target.
		bool AttemptWrapRecovery(bool& recovered_at_target);
		std::optional<NavKeyCommand> GiveUpCursorMove();

		// Content-based line resolution: find the screen line whose text starts with the given label
		std::optional<uint8_t> FindLineByLabel(const std::string& label) const;

		// Handle password entry page
		std::optional<NavKeyCommand> HandlePasswordEntry();

		// Handle special pages (Service, Timeout) that block normal navigation
		bool HandleSpecialPage(PageId page);

		// Check if current page is a blocking special page
		bool IsBlockingPage(PageId page) const;

		// Check if a page is a transient splash/cold-start page that auto-advances
		bool IsTransientPage(PageId page) const;

	private:
		const MenuModel& m_Model;
		State m_State{ State::Idle };

		PageId m_CurrentPage{ PageId::Unknown };
		PageId m_TargetPage{ PageId::Unknown };
		uint8_t m_CursorLine{ 0 };
		uint8_t m_TargetCursorLine{ 0 };

		std::vector<const MenuEdge*> m_Path;
		size_t m_PathIndex{ 0 };

		// Status message tracking
		uint32_t m_PendingStatusMessages{ 0 };

		// Recovery state
		uint32_t m_RecoveryAttempts{ 0 };
		uint32_t m_RecoveryBackPresses{ 0 };

		// For item navigation
		bool m_NavigatingToItem{ false };
		uint8_t m_TargetItemLine{ 0 };
		std::string m_ItemLabel;                   // Label for content-based item discovery
		PageId m_SelectTarget{ PageId::Unknown };  // Page expected after Select (multi-instance sub-page)
		uint32_t m_ItemScrollAttempts{ 0 };         // PageDown attempts for scrollable lists
		static constexpr uint32_t MAX_ITEM_SCROLL_ATTEMPTS = 6;  // Max scrolls before giving up

		// Cursor movement tracking (to detect stuck cursors at screen boundaries)
		uint8_t m_PreviousCursorLine{ 0 };
		uint32_t m_CursorStuckCount{ 0 };
		static constexpr uint32_t MAX_CURSOR_STUCK_COUNT{ 5 };
		bool m_SkipCursorCheck{ false };  // Skip cursor check after accepting stuck position

		// Content-based cursor targeting
		const Utility::ScreenDataPage* m_pCurrentContent{ nullptr };  // Screen content during OnPageUpdate
		const MenuEdge* m_CurrentEdge{ nullptr };                      // Edge being navigated (for recovery)
		uint32_t m_CursorMoveCount{ 0 };                               // Wrap detection counter

		// Password entry support
		std::string m_Password;           // 4-digit password for protected menus
		uint8_t m_PasswordDigitIndex{ 0 };  // Current digit being entered

		// Timeout tracking
		uint32_t m_WaitCycleCount{ 0 };   // Cycles waiting in same state

		// Recompute tracking (to prevent infinite loops)
		uint32_t m_RecomputeCount{ 0 };

		// Stuck-recompute tracking: when a target page's menu item does not exist on this
		// model (e.g. an IAQ-only Diagnostics page on a non-iAqualink panel), the cursor
		// gets stuck, the Navigator "proceeds" onto a wrong page, recomputes the SAME
		// deterministic failing path, and repeats. Detecting repeated recomputes that land
		// on the same page for the same target fails the navigation fast (after
		// MAX_STUCK_RECOMPUTES) instead of grinding to MAX_RECOMPUTE_COUNT.
		PageId m_LastRecomputeActual{ PageId::Unknown };
		PageId m_LastRecomputeTarget{ PageId::Unknown };
		uint32_t m_StuckRecomputeCount{ 0 };

		// Transient-page wait tracking (the cold-start splash auto-advances on its own)
		uint32_t m_TransientWaitCount{ 0 };

		// Sync state tracking
		PageId m_SyncDetectedPage{ PageId::Unknown };
		uint32_t m_SyncConsistentCount{ 0 };
	};

}
// namespace AqualinkAutomate::Navigation
