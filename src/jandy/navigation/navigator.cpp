#include "navigation/navigator.h"

#include <cctype>
#include <source_location>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Navigation
{

	Navigator::Navigator(const MenuModel& model)
		: m_Model(model)
	{
	}

	void Navigator::StartSync()
	{
		LogInfo(Channel::Navigation, "Navigator: Starting sync - will detect current page via consecutive consistent detections");
		m_State = State::Syncing;
		m_CurrentPage = PageId::Unknown;
		m_SyncDetectedPage = PageId::Unknown;
		m_SyncConsistentCount = 0;
		m_TransientWaitCount = 0;
	}

	void Navigator::NavigateTo(PageId target)
	{
		const MenuPage* target_page = m_Model.GetPage(target);
		const MenuPage* current_page_info = m_Model.GetPage(m_CurrentPage);

		// Track navigation attempts for diagnostics
		static uint32_t s_TotalNavigationAttempts = 0;
		s_TotalNavigationAttempts++;

		LogInfo(Channel::Navigation, [&target, &target_page, &current_page_info, this] { return std::format("Navigator: Starting navigation #{} to page {}({}) from {}({})",
			s_TotalNavigationAttempts,
			std::to_underlying(target), target_page ? target_page->name : "Unknown",
			std::to_underlying(m_CurrentPage), current_page_info ? current_page_info->name : "Unknown"); });

		m_TargetPage = target;
		m_NavigatingToItem = false;
		m_SelectTarget = PageId::Unknown;
		m_PathIndex = 0;
		m_Path.clear();
		m_State = State::Navigating;
		m_PendingStatusMessages = 0;
		m_RecomputeCount = 0;
		m_StuckRecomputeCount = 0;
		m_LastRecomputeActual = PageId::Unknown;
		m_LastRecomputeTarget = PageId::Unknown;
		m_RecoveryAttempts = 0;
		m_RecoveryBackPresses = 0;
		m_TransientWaitCount = 0;
	}

	void Navigator::NavigateToItem(PageId page, uint8_t menu_item_line, const std::string& item_label, PageId select_target)
	{
		LogInfo(Channel::Navigation, [&page, &menu_item_line, &item_label, &select_target] { return std::format("Navigator: Starting navigation to item on page {}, line {}, label '{}', select_target={}",
			std::to_underlying(page), menu_item_line, item_label, std::to_underlying(select_target)); });

		m_TargetPage = page;
		m_NavigatingToItem = true;
		m_TargetItemLine = menu_item_line;
		m_ItemLabel = item_label;
		m_SelectTarget = select_target;
		m_ItemScrollAttempts = 0;
		m_PathIndex = 0;
		m_Path.clear();
		m_State = State::Navigating;
		m_PendingStatusMessages = 0;
		m_RecomputeCount = 0;
		m_StuckRecomputeCount = 0;
		m_LastRecomputeActual = PageId::Unknown;
		m_LastRecomputeTarget = PageId::Unknown;
		m_RecoveryAttempts = 0;
		m_RecoveryBackPresses = 0;
		m_TransientWaitCount = 0;
	}

	void Navigator::OnStatusMessageReceived()
	{
		if (m_PendingStatusMessages > 0)
		{
			m_PendingStatusMessages--;
			LogTrace(Channel::Navigation, [this] { return std::format("Navigator: Status received, {} pending",
				m_PendingStatusMessages); });
		}
	}

	std::optional<NavKeyCommand> Navigator::OnPageUpdate(
		const Utility::ScreenDataPage& content,
		uint8_t highlighted_line)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Navigator::OnPageUpdate", std::source_location::current());

		// Store screen content pointer for content-based line resolution
		m_pCurrentContent = &content;

		// Update cursor position from highlight. A clear-all sentinel (0xFF) means no line is
		// highlighted on the device; treating it as a literal row index would corrupt cursor
		// targeting, so retain the previously known cursor line in that case.
		if (highlighted_line != CURSOR_LINE_NONE)
		{
			m_CursorLine = highlighted_line;
		}
		else
		{
			LogTrace(Channel::Navigation, "Navigator: Clear-all highlight (0xFF) received; retaining previous cursor line");
		}

		// Detect the current page once for this content update; the result is shared with the
		// Syncing handler below so DetectPage is not run twice for the same screen.
		const PageId detected = m_Model.DetectPage(content);

		// Handle Syncing state separately (before page detection updates m_CurrentPage)
		if (m_State == State::Syncing)
		{
			return HandleSyncing(detected);
		}

		// Try to detect current page from content
		if (detected != PageId::Unknown)
		{
			m_CurrentPage = detected;
		}

		// Get page names for logging
		const MenuPage* current_page_info = m_Model.GetPage(m_CurrentPage);
		const MenuPage* target_page_info = m_Model.GetPage(m_TargetPage);

		LogTrace(Channel::Navigation, [this, &current_page_info, &target_page_info] { return std::format("Navigator: Page update - state={}, current={}({}), target={}({}), cursor={}, pending={}",
			magic_enum::enum_name(m_State),
			std::to_underlying(m_CurrentPage),
			current_page_info ? current_page_info->name : "Unknown",
			std::to_underlying(m_TargetPage),
			target_page_info ? target_page_info->name : "Unknown",
			m_CursorLine,
			m_PendingStatusMessages); });

		// Log screen content at trace level for debugging
		if (m_State == State::Navigating || m_State == State::WaitingForPage || m_State == State::Reorienting)
		{
			LogTrace(Channel::Navigation, "Navigator: Screen content:");
			for (size_t i = 0; i < content.Size() && i < 12; ++i)
			{
				const auto& row = content[i];
				if (!row.Text.empty())
				{
					LogTrace(Channel::Navigation, [&i, &row, &highlighted_line] { return std::format("  Line {}: '{}' {}",
						i, row.Text,
						(i == highlighted_line) ? "<-- CURSOR" : ""); });
				}
			}
		}

		// Check for special blocking pages (Service, Timeout)
		if (IsBlockingPage(m_CurrentPage))
		{
			if (!HandleSpecialPage(m_CurrentPage))
			{
				return std::nullopt;
			}
		}

		// Check for password prompt page
		if (m_CurrentPage == PageId::EnterPassword && m_State != State::EnteringPassword)
		{
			if (m_Password.empty())
			{
				LogWarning(Channel::Navigation, "Navigator: Encountered password prompt but no password configured - backing out");
				m_PendingStatusMessages = 2;
				return NavKeyCommand::Back;
			}
			else
			{
				// Automated PIN entry over RS-485 is not yet supported: it requires reading the
				// current on-screen digit value to know how many LineUp/LineDown presses to send,
				// which has not been reverse-engineered. Rather than press Select blindly (which
				// previously *reported success* without ever setting a digit), back out honestly.
				// The configured PIN value is deliberately NOT logged.
				LogError(Channel::Navigation, "Navigator: Encountered password prompt - automated password entry is not supported, backing out");
				m_PendingStatusMessages = 2;
				return NavKeyCommand::Back;
			}
		}

		// Track wait cycles for timeout detection
		if (m_State == State::WaitingForPage && m_PendingStatusMessages > 0)
		{
			m_WaitCycleCount++;
			if (m_WaitCycleCount >= MAX_WAIT_CYCLES)
			{
				LogError(Channel::Navigation, [this] { return std::format("Navigator: Timeout waiting for page response after {} cycles",
					m_WaitCycleCount); });
				m_WaitCycleCount = 0;
				InitiateRecovery();
				return RecoveryNext();
			}
		}
		else
		{
			m_WaitCycleCount = 0;  // Reset on progress
		}

		switch (m_State)
		{
		case State::Idle:
			[[fallthrough]];
		case State::AtDestination:
			[[fallthrough]];
		case State::Failed:
			return std::nullopt;

		case State::WaitingForPage:
			return HandleWaitingForPage();

		case State::MovingCursor:
			return MoveCursorToTarget();

		case State::Navigating:
			return ExecuteNextStep();

		case State::Reorienting:
			return RecoveryNext();

		case State::EnteringPassword:
			return HandlePasswordEntry();

		case State::Syncing:
			// Already handled above, shouldn't reach here
			return std::nullopt;
		}

		return std::nullopt;
	}

	std::optional<NavKeyCommand> Navigator::HandleSyncing(PageId detected)
	{
		if (detected == PageId::Unknown)
		{
			// No detection - reset consistency counter
			m_SyncConsistentCount = 0;
			m_SyncDetectedPage = PageId::Unknown;
			LogTrace(Channel::Navigation, "Navigator: Sync - no page detected, waiting...");
			return std::nullopt;
		}

		// Skip transient pages during sync - these are not navigable starting points
		if (detected == PageId::StartUp || detected == PageId::Service || detected == PageId::TimeOut)
		{
			const MenuPage* page_info = m_Model.GetPage(detected);
			LogDebug(Channel::Navigation, [&detected, &page_info] { return std::format("Navigator: Sync - transient page {}({}) detected, waiting for navigable page...",
				std::to_underlying(detected), page_info ? page_info->name : "Unknown"); });
			m_SyncConsistentCount = 0;
			m_SyncDetectedPage = PageId::Unknown;
			return std::nullopt;
		}

		if (detected == m_SyncDetectedPage)
		{
			// Same page detected again
			m_SyncConsistentCount++;
			LogTrace(Channel::Navigation, [&detected, this] { return std::format("Navigator: Sync - page {} detected consistently ({}/{})",
				std::to_underlying(detected), m_SyncConsistentCount, SYNC_REQUIRED_CONSISTENT_COUNT); });
		}
		else
		{
			// Different page detected - restart counter
			const MenuPage* page_info = m_Model.GetPage(detected);
			LogDebug(Channel::Navigation, [&detected, &page_info] { return std::format("Navigator: Sync - new page detected: {}({}), restarting consistency counter",
				std::to_underlying(detected), page_info ? page_info->name : "Unknown"); });
			m_SyncDetectedPage = detected;
			m_SyncConsistentCount = 1;
		}

		if (m_SyncConsistentCount >= SYNC_REQUIRED_CONSISTENT_COUNT)
		{
			// Sync complete
			m_CurrentPage = m_SyncDetectedPage;
			const MenuPage* page_info = m_Model.GetPage(m_CurrentPage);
			LogInfo(Channel::Navigation, [this, &page_info] { return std::format("Navigator: Sync complete - current page is {}({})",
				std::to_underlying(m_CurrentPage), page_info ? page_info->name : "Unknown"); });
			m_State = State::Idle;
			m_SyncDetectedPage = PageId::Unknown;
			m_SyncConsistentCount = 0;
		}

		return std::nullopt;
	}

	bool Navigator::IsComplete() const
	{
		return m_State == State::AtDestination || m_State == State::Failed;
	}

	void Navigator::Reset()
	{
		using enum PageId;

		LogDebug(Channel::Navigation, "Navigator: Resetting to idle state");
		m_State = State::Idle;
		m_CurrentPage = Unknown;
		m_TargetPage = Unknown;
		m_Path.clear();
		m_PathIndex = 0;
		m_PendingStatusMessages = 0;
		m_RecoveryAttempts = 0;
		m_RecoveryBackPresses = 0;
		m_NavigatingToItem = false;
		m_SelectTarget = Unknown;
		m_CursorStuckCount = 0;
		m_PreviousCursorLine = 0;
		m_SkipCursorCheck = false;
		m_PasswordDigitIndex = 0;
		m_WaitCycleCount = 0;
		m_RecomputeCount = 0;
		m_StuckRecomputeCount = 0;
		m_LastRecomputeActual = Unknown;
		m_LastRecomputeTarget = Unknown;
		m_TransientWaitCount = 0;
		m_SyncDetectedPage = Unknown;
		m_SyncConsistentCount = 0;
		m_pCurrentContent = nullptr;
		m_CurrentEdge = nullptr;
		m_CursorMoveCount = 0;
	}

	void Navigator::ComputePath()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Navigator::ComputePath", std::source_location::current());

		if (m_CurrentPage == PageId::Unknown)
		{
			LogWarning(Channel::Navigation, "Navigator: Cannot compute path - current page unknown");
			InitiateRecovery();
			return;
		}

		const MenuPage* from_page = m_Model.GetPage(m_CurrentPage);
		const MenuPage* to_page = m_Model.GetPage(m_TargetPage);

		LogTrace(Channel::Navigation, [this, &from_page, &to_page] { return std::format("Navigator: Computing path from {}({}) to {}({})",
			std::to_underlying(m_CurrentPage), from_page ? from_page->name : "Unknown",
			std::to_underlying(m_TargetPage), to_page ? to_page->name : "Unknown"); });

		m_Path = m_Model.FindPath(m_CurrentPage, m_TargetPage);
		m_PathIndex = 0;

		if (m_Path.empty() && m_CurrentPage != m_TargetPage)
		{
			LogTrace(Channel::Navigation, [this] { return std::format("Navigator: No path found from {} to {}",
				std::to_underlying(m_CurrentPage), std::to_underlying(m_TargetPage)); });
			InitiateRecovery();
			return;
		}

		LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Computed path with {} steps",
			m_Path.size()); });

		// Log each step in the path
		for (size_t i = 0; i < m_Path.size(); ++i)
		{
			const MenuEdge* edge = m_Path[i];
			const MenuPage* step_from = m_Model.GetPage(edge->source);
			const MenuPage* step_to = m_Model.GetPage(edge->target);

			LogTrace(Channel::Navigation, [&i, &edge, &step_from, &step_to] { return std::format("  Step {}: {} from {}({}) to {}({}) target_line={} '{}'",
				i,
				magic_enum::enum_name(edge->trigger),
				std::to_underlying(edge->source), step_from ? step_from->name : "Unknown",
				std::to_underlying(edge->target), step_to ? step_to->name : "Unknown",
				edge->trigger_line, edge->label); });
		}
	}

	std::optional<NavKeyCommand> Navigator::HandleWaitingForPage()
	{
		// Still waiting for status messages
		if (m_PendingStatusMessages > 0)
		{
			LogTrace(Channel::Navigation, [this] { return std::format("Navigator: HandleWaitingForPage - still waiting for {} status messages",
				m_PendingStatusMessages); });
			return std::nullopt;
		}

		// Check if we arrived at expected page
		if (m_PathIndex > 0 && m_PathIndex <= m_Path.size())
		{
			const MenuEdge* last_edge = m_Path[m_PathIndex - 1];
			const MenuPage* expected_page = m_Model.GetPage(last_edge->target);
			const MenuPage* current_page = m_Model.GetPage(m_CurrentPage);

			LogTrace(Channel::Navigation, [&last_edge, &expected_page, this, &current_page] { return std::format("Navigator: HandleWaitingForPage - checking arrival: expected={}({}), actual={}({})",
				std::to_underlying(last_edge->target), expected_page ? expected_page->name : "Unknown",
				std::to_underlying(m_CurrentPage), current_page ? current_page->name : "Unknown"); });

			if (m_CurrentPage != last_edge->target && last_edge->target != PageId::Unknown)
			{
				LogWarning(Channel::Navigation, [&last_edge, &expected_page, this, &current_page] { return std::format("Navigator: Unexpected page after navigation - expected {}({}), got {}({})",
					std::to_underlying(last_edge->target), expected_page ? expected_page->name : "Unknown",
					std::to_underlying(m_CurrentPage), current_page ? current_page->name : "Unknown"); });
				HandleUnexpectedPage(m_CurrentPage);

				// HandleUnexpectedPage may have recomputed a valid path (state=Navigating)
				// or initiated recovery (state=Reorienting) or given up (state=Failed).
				if (m_State == State::Navigating)
				{
					return ExecuteNextStep();
				}
				else if (m_State == State::Reorienting)
				{
					return RecoveryNext();
				}
				return std::nullopt;
			}
		}

		// Continue navigation
		LogTrace(Channel::Navigation, "Navigator: HandleWaitingForPage - page arrived as expected, continuing");
		m_State = State::Navigating;
		return ExecuteNextStep();
	}

	std::optional<NavKeyCommand> Navigator::ExecuteNextStep()
	{
		// If we don't have a path yet, compute one
		if (m_Path.empty() && m_CurrentPage != m_TargetPage)
		{
			ComputePath();
			if (m_State == State::Reorienting)
			{
				return RecoveryNext();
			}
		}

		// Check if we've reached the target page
		if (m_CurrentPage == m_TargetPage)
		{
			if (m_NavigatingToItem)
			{
				// Use content-based label matching to find the item on screen
				if (!m_ItemLabel.empty())
				{
					auto resolved = FindLineByLabel(m_ItemLabel);
					if (resolved.has_value())
					{
						// Found the item on the current screen
						m_TargetCursorLine = resolved.value();
						if (m_CursorLine != m_TargetCursorLine)
						{
							m_State = State::MovingCursor;
							return MoveCursorToTarget();
						}

						// Cursor is positioned on the item. If we have a select_target,
						// send Select to enter the sub-page.
						if (m_SelectTarget != PageId::Unknown)
						{
							LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Item '{}' found at line {}, sending Select to enter page {}",
								m_ItemLabel, m_CursorLine, std::to_underlying(m_SelectTarget)); });
							m_TargetPage = m_SelectTarget;
							m_NavigatingToItem = false;
							m_SelectTarget = PageId::Unknown;
							m_Path.clear();
							m_PathIndex = 0;
							m_PendingStatusMessages = 2;
							m_State = State::WaitingForPage;
							return NavKeyCommand::Select;
						}
					}
					else
					{
						// Item not on screen -- try scrolling down if the page supports it
						const MenuPage* page = m_Model.GetPage(m_CurrentPage);
						if (page && page->SupportsKey(EdgeTrigger::PageDown) && m_ItemScrollAttempts < MAX_ITEM_SCROLL_ATTEMPTS)
						{
							m_ItemScrollAttempts++;
							// Log screen content on first scroll attempt for diagnostics
							if (m_ItemScrollAttempts == 1 && m_pCurrentContent)
							{
								LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Item '{}' not on initial screen, dumping content:", m_ItemLabel); });
								for (size_t di = 0; di < m_pCurrentContent->Size() && di < 12; ++di)
								{
									const auto& drow = (*m_pCurrentContent)[di];
									if (!drow.Text.empty())
									{
										LogWarning(Channel::Navigation, [&di, &drow] { return std::format("Navigator: Screen[{}]: '{}'", di, drow.Text); });
									}
								}
							}
							LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Item '{}' not visible, scrolling down (attempt {}/{})",
								m_ItemLabel, m_ItemScrollAttempts, MAX_ITEM_SCROLL_ATTEMPTS); });
							m_PendingStatusMessages = 1;
							m_State = State::WaitingForPage;
							return NavKeyCommand::PageDown_Or_Select1;
						}
						else
						{
							// Dump screen content for debugging
							if (m_pCurrentContent)
							{
								for (size_t di = 0; di < m_pCurrentContent->Size() && di < 12; ++di)
								{
									const auto& drow = (*m_pCurrentContent)[di];
									if (!drow.Text.empty())
									{
										LogWarning(Channel::Navigation, [&di, &drow] { return std::format("Navigator: Screen[{}]: '{}'", di, drow.Text); });
									}
								}
							}
							// Item not found after scrolling -- doesn't exist on this controller
							LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Item '{}' not found on page {} -- failing navigation (scrolled {} times)",
								m_ItemLabel, std::to_underlying(m_CurrentPage), m_ItemScrollAttempts); });
							m_State = State::Failed;
							return std::nullopt;
						}
					}
				}
				else
				{
					// Fallback: use fixed line number
					m_TargetCursorLine = m_TargetItemLine;
					if (m_CursorLine != m_TargetCursorLine)
					{
						m_State = State::MovingCursor;
						return MoveCursorToTarget();
					}

					// Cursor is positioned. If we have a select_target, send Select.
					if (m_SelectTarget != PageId::Unknown)
					{
						LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Item at line {}, sending Select to enter page {}",
							m_CursorLine, std::to_underlying(m_SelectTarget)); });
						m_TargetPage = m_SelectTarget;
						m_NavigatingToItem = false;
						m_SelectTarget = PageId::Unknown;
						m_Path.clear();
						m_PathIndex = 0;
						m_PendingStatusMessages = 2;
						m_State = State::WaitingForPage;
						return NavKeyCommand::Select;
					}
				}
			}

			LogInfo(Channel::Navigation, [this] { return std::format("Navigator: Reached destination page {}",
				std::to_underlying(m_TargetPage)); });
			m_State = State::AtDestination;
			return std::nullopt;
		}

		// Check if path is exhausted
		if (m_PathIndex >= m_Path.size())
		{
			LogWarning(Channel::Navigation, "Navigator: Path exhausted but not at destination");
			// Recompute path
			m_Path.clear();
			return ExecuteNextStep();
		}

		const MenuEdge* edge = m_Path[m_PathIndex];
		const MenuPage* step_from = m_Model.GetPage(edge->source);
		const MenuPage* step_to = m_Model.GetPage(edge->target);

		LogTrace(Channel::Navigation, [this, &edge, &step_from, &step_to] { return std::format("Navigator: Executing step {}/{}: {} from {}({}) to {}({}), target_line={}, cursor={}, '{}'",
			m_PathIndex, m_Path.size(),
			magic_enum::enum_name(edge->trigger),
			std::to_underlying(edge->source), step_from ? step_from->name : "Unknown",
			std::to_underlying(edge->target), step_to ? step_to->name : "Unknown",
			edge->trigger_line, m_CursorLine, edge->label); });

		// First, ensure cursor is at the right position for Select steps
		// Skip this check if we just accepted a stuck cursor position
		if (edge->trigger == EdgeTrigger::Select && !m_SkipCursorCheck)
		{
			// Content-based line resolution: use label to find actual screen line
			uint8_t target_line = edge->trigger_line;
			if (!edge->label.empty())
			{
				auto resolved = FindLineByLabel(edge->label);
				if (resolved.has_value())
				{
					if (resolved.value() != edge->trigger_line)
					{
						LogDebug(Channel::Navigation, [&edge, &resolved] { return std::format("Navigator: Content-based resolution: label '{}' at line {} (model says {})",
							edge->label, resolved.value(), edge->trigger_line); });
					}
					target_line = resolved.value();
				}
			}

			if (m_CursorLine != target_line)
			{
				LogTrace(Channel::Navigation, [this, &target_line] { return std::format("Navigator: Need to move cursor from {} to {} before Select",
					m_CursorLine, target_line); });
				m_TargetCursorLine = target_line;
				m_CurrentEdge = edge;
				m_CursorMoveCount = 0;
				m_State = State::MovingCursor;
				return MoveCursorToTarget();
			}
		}
		m_SkipCursorCheck = false;  // Reset flag after checking

		// Execute the step
		m_PathIndex++;
		NavKeyCommand cmd = NavKeyCommand::NoCommand;

		switch (edge->trigger)
		{
		case EdgeTrigger::Select:
			LogDebug(Channel::Navigation, [&edge, &step_to] { return std::format("Navigator: EXECUTING Select at line {} to go to page {}({}) '{}'",
				edge->trigger_line, std::to_underlying(edge->target), step_to ? step_to->name : "Unknown", edge->label); });
			cmd = NavKeyCommand::Select;
			// Page transitions need 2 status messages
			m_PendingStatusMessages = 2;
			break;

		case EdgeTrigger::Back:
			LogDebug(Channel::Navigation, [&edge, &step_to] { return std::format("Navigator: EXECUTING Back to go to page {}({})",
				std::to_underlying(edge->target), step_to ? step_to->name : "Unknown"); });
			cmd = NavKeyCommand::Back;
			// Back also causes page transition
			m_PendingStatusMessages = 2;
			break;

		case EdgeTrigger::LineUp:
			LogTrace(Channel::Navigation, "Navigator: EXECUTING LineUp");
			cmd = NavKeyCommand::LineUp;
			m_PendingStatusMessages = 1;
			break;

		case EdgeTrigger::LineDown:
			LogTrace(Channel::Navigation, "Navigator: EXECUTING LineDown");
			cmd = NavKeyCommand::LineDown;
			m_PendingStatusMessages = 1;
			break;

		case EdgeTrigger::PageUp:
			LogTrace(Channel::Navigation, "Navigator: EXECUTING PageUp");
			cmd = NavKeyCommand::PageUp_Or_Select3;
			m_PendingStatusMessages = 1;
			break;

		case EdgeTrigger::PageDown:
			LogTrace(Channel::Navigation, "Navigator: EXECUTING PageDown");
			cmd = NavKeyCommand::PageDown_Or_Select1;
			m_PendingStatusMessages = 1;
			break;

		case EdgeTrigger::SystemTimeout:
		case EdgeTrigger::SystemService:
			// These are involuntary transitions, not commands to send
			LogWarning(Channel::Navigation, "Navigator: Attempted to execute system event edge as a command");
			break;
		}

		m_State = State::WaitingForPage;
		return cmd;
	}

	std::optional<NavKeyCommand> Navigator::MoveCursorToTarget()
	{
		if (m_CursorLine == m_TargetCursorLine)
		{
			// Cursor is at target, continue navigation
			m_CursorStuckCount = 0;
			m_CursorMoveCount = 0;
			m_State = State::Navigating;
			return ExecuteNextStep();
		}

		// Still waiting for previous move to complete
		if (m_PendingStatusMessages > 0)
		{
			return std::nullopt;
		}

		// Wrap detection: too many moves without reaching target. Try a one-shot content-based
		// retarget; if that does not place us on (or move us toward) a reachable line, give up.
		m_CursorMoveCount++;
		if (m_CursorMoveCount > MAX_CURSOR_MOVES)
		{
			LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Cursor wrap detected after {} moves (cursor={}, target={})",
				m_CursorMoveCount, m_CursorLine, m_TargetCursorLine); });

			bool recovered_at_target = false;
			if (AttemptWrapRecovery(recovered_at_target))
			{
				if (recovered_at_target)
				{
					m_CursorStuckCount = 0;
					m_State = State::Navigating;
					return ExecuteNextStep();
				}
				// Recovery retargeted to a different reachable line; fall through to move the cursor.
			}
			else
			{
				// No usable recovery target - abandon the move.
				return GiveUpCursorMove();
			}
		}

		// Check if cursor is stuck (didn't move from previous position)
		if (m_CursorLine == m_PreviousCursorLine && m_CursorStuckCount > 0)
		{
			m_CursorStuckCount++;
			LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Cursor stuck at line {} (attempt {}/{}), target is {}",
				m_CursorLine, m_CursorStuckCount, MAX_CURSOR_STUCK_COUNT, m_TargetCursorLine); });

			if (m_CursorStuckCount >= MAX_CURSOR_STUCK_COUNT)
			{
				// For NavigateToItem mode, fail cleanly -- the target item doesn't exist
				if (m_NavigatingToItem)
				{
					LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Cursor stuck at line {} in NavigateToItem mode - failing navigation",
						m_CursorLine); });
					m_CursorStuckCount = 0;
					m_State = State::Failed;
					return std::nullopt;
				}

				// Cursor appears to be at a boundary and can't move further
				// Assume we're as close as we can get and proceed
				LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Cursor stuck at line {} after {} attempts, assuming at boundary and proceeding",
					m_CursorLine, m_CursorStuckCount); });
				m_CursorStuckCount = 0;
				m_TargetCursorLine = m_CursorLine;  // Accept current position as target
				m_SkipCursorCheck = true;  // Skip cursor position check in ExecuteNextStep
				m_State = State::Navigating;
				return ExecuteNextStep();
			}
		}
		else
		{
			// Cursor moved or this is the first attempt
			m_CursorStuckCount = 1;
		}

		// Remember current position to detect if next move fails
		m_PreviousCursorLine = m_CursorLine;

		// Move cursor towards target
		NavKeyCommand cmd = NavKeyCommand::LineDown;
		if (m_CursorLine < m_TargetCursorLine)
		{
			LogTrace(Channel::Navigation, [this] { return std::format("Navigator: Moving cursor down from {} to {}",
				m_CursorLine, m_TargetCursorLine); });
			cmd = NavKeyCommand::LineDown;
		}
		else
		{
			LogTrace(Channel::Navigation, [this] { return std::format("Navigator: Moving cursor up from {} to {}",
				m_CursorLine, m_TargetCursorLine); });
			cmd = NavKeyCommand::LineUp;
		}

		m_PendingStatusMessages = 1;
		return cmd;
	}

	bool Navigator::AttemptWrapRecovery(bool& recovered_at_target)
	{
		recovered_at_target = false;

		// Try content-based recovery (the screen may have updated since ExecuteNextStep set the
		// model-derived target line). If the label now resolves to a different line, retarget to it.
		if (m_CurrentEdge && !m_CurrentEdge->label.empty())
		{
			auto resolved = FindLineByLabel(m_CurrentEdge->label);
			if (resolved.has_value() && resolved.value() != m_TargetCursorLine)
			{
				LogInfo(Channel::Navigation, [this, &resolved] { return std::format("Navigator: Wrap recovery: retargeting from line {} to {} via label '{}'",
					m_TargetCursorLine, resolved.value(), m_CurrentEdge->label); });
				m_TargetCursorLine = resolved.value();
				m_CursorMoveCount = 0;
				recovered_at_target = (m_CursorLine == m_TargetCursorLine);
				return true;
			}
		}

		return false;
	}

	std::optional<NavKeyCommand> Navigator::GiveUpCursorMove()
	{
		m_CursorMoveCount = 0;

		// For NavigateToItem mode, fail cleanly -- the target item likely doesn't exist on the
		// screen (e.g. "AUX B1" on a controller without power center B).
		if (m_NavigatingToItem)
		{
			LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Target line {} unreachable in NavigateToItem mode - failing navigation",
				m_TargetCursorLine); });
			m_State = State::Failed;
			return std::nullopt;
		}

		LogWarning(Channel::Navigation, "Navigator: Accepting current cursor position after wrap detection");
		m_TargetCursorLine = m_CursorLine;
		m_SkipCursorCheck = true;
		m_State = State::Navigating;
		return ExecuteNextStep();
	}

	bool Navigator::HandleTransientPage(PageId actual, const MenuPage* actual_page)
	{
		// Transient pages (e.g. the cold-start StartUp splash) auto-advance to a real
		// page on their own with no keypress. They have no edges, so navigating or
		// recovering from them is impossible and previously failed the whole startup
		// crawl. Instead, WAIT for the controller to transition and re-evaluate on the
		// next page update. Bounded by MAX_TRANSIENT_WAITS so a page that never clears
		// falls through to normal unexpected-page handling/recovery.
		if ((actual == PageId::Unknown) || !IsTransientPage(actual))
		{
			return false;
		}

		m_CurrentPage = actual;
		if (++m_TransientWaitCount <= MAX_TRANSIENT_WAITS)
		{
			LogInfo(Channel::Navigation, [&actual, &actual_page, this] { return std::format("Navigator: On transient page {}({}); waiting for controller auto-transition ({}/{})",
				std::to_underlying(actual), actual_page ? actual_page->name : "Unknown",
				m_TransientWaitCount, MAX_TRANSIENT_WAITS); });
			m_State = State::WaitingForPage;
			m_PendingStatusMessages = 0;
			return true;
		}

		LogWarning(Channel::Navigation, [&actual, &actual_page] { return std::format("Navigator: Transient page {}({}) did not clear after {} waits; treating as unexpected",
			std::to_underlying(actual), actual_page ? actual_page->name : "Unknown", MAX_TRANSIENT_WAITS); });
		return false;
	}

	bool Navigator::DetectStuckRecompute(PageId actual, const MenuPage* target_page)
	{
		// Fail FAST when a target is unreachable on this model: repeatedly landing on an
		// unexpected page while navigating to the SAME target means that target's menu item
		// does not exist on this revision (e.g. an IAQ-only Diagnostics page on a non-iAqualink
		// panel, or the chlorinator Boost/Aquapure pages on a panel with no SWG). The cursor
		// wraps past the missing menu line, we "proceed" onto a wrong page, recompute, and
		// repeat - and the wrong page it lands on can VARY from cycle to cycle, so key the
		// stuck detector on the TARGET alone (not on actual+target, which resets whenever the
		// wrong landing page changes and lets the loop grind all the way to MAX_RECOMPUTE_COUNT).
		// Detect it after a few cycles instead, so the start-up scrape skips the missing page
		// and moves on rather than wedging for ~30-50 polls/page.
		if (m_TargetPage == m_LastRecomputeTarget)
		{
			if (++m_StuckRecomputeCount >= MAX_STUCK_RECOMPUTES)
			{
				LogWarning(Channel::Navigation, [this, &target_page, &actual] { return std::format("Navigator: Target {}({}) appears unreachable on this model (recomputed {} times without arriving, last landed on {}) - failing fast",
					std::to_underlying(m_TargetPage), target_page ? target_page->name : "Unknown",
					m_StuckRecomputeCount, std::to_underlying(actual)); });
				m_StuckRecomputeCount = 0;
				m_LastRecomputeActual = PageId::Unknown;
				m_LastRecomputeTarget = PageId::Unknown;
				m_State = State::Failed;
				return true;
			}
		}
		else
		{
			m_StuckRecomputeCount = 1;
			m_LastRecomputeTarget = m_TargetPage;
		}
		m_LastRecomputeActual = actual;
		return false;
	}

	void Navigator::HandleUnexpectedPage(PageId actual)
	{
		const MenuPage* actual_page = m_Model.GetPage(actual);
		const MenuPage* target_page = m_Model.GetPage(m_TargetPage);

		if (HandleTransientPage(actual, actual_page))
		{
			return;
		}

		LogWarning(Channel::Navigation, [&actual, &actual_page, this, &target_page] { return std::format("Navigator: Handling unexpected page {}({}) while navigating to {}({})",
			std::to_underlying(actual), actual_page ? actual_page->name : "Unknown",
			std::to_underlying(m_TargetPage), target_page ? target_page->name : "Unknown"); });

		// Track recomputation attempts to detect potential infinite recompute loops
		m_RecomputeCount++;
		if (m_RecomputeCount % 10 == 0)
		{
			LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Path recomputed {} times - possible navigation loop",
				m_RecomputeCount); });
		}

		if (DetectStuckRecompute(actual, target_page))
		{
			return;
		}

		// Check for infinite recompute loop
		if (m_RecomputeCount >= MAX_RECOMPUTE_COUNT)
		{
			LogError(Channel::Navigation, [] { return std::format("Navigator: Max recompute count ({}) exceeded - navigation loop detected, failing",
				MAX_RECOMPUTE_COUNT); });
			m_State = State::Failed;
			return;
		}

		// Check for system events (timeout, service mode)
		if (auto system_event = m_Model.FindSystemEvent(actual); system_event.has_value())
		{
			LogWarning(Channel::Navigation, [&system_event] { return std::format("Navigator: System event detected: '{}'",
				system_event->label); });
			// Let the blocking page handler deal with it
		}

		// If we recognize the page, try to recompute path from here
		if (actual != PageId::Unknown)
		{
			m_CurrentPage = actual;
			m_Path.clear();
			m_PathIndex = 0;

			// Try to compute a new path
			m_Path = m_Model.FindPath(m_CurrentPage, m_TargetPage);
			if (!m_Path.empty())
			{
				LogInfo(Channel::Navigation, [this, &actual_page] { return std::format("Navigator: Recomputed path from {}({}) with {} steps",
					std::to_underlying(m_CurrentPage), actual_page ? actual_page->name : "Unknown", m_Path.size()); });
				m_State = State::Navigating;
				return;
			}
		}

		// Cannot recover from current position, initiate full recovery
		InitiateRecovery();
	}

	void Navigator::InitiateRecovery()
	{
		m_RecoveryAttempts++;
		LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Initiating recovery, attempt {}/{}",
			m_RecoveryAttempts, MAX_RECOVERY_ATTEMPTS); });

		if (m_RecoveryAttempts > MAX_RECOVERY_ATTEMPTS)
		{
			LogError(Channel::Navigation, "Navigator: Max recovery attempts exceeded - entering Failed state");
			m_State = State::Failed;
			return;
		}

		m_State = State::Reorienting;
		m_RecoveryBackPresses = 0;
		m_Path.clear();
		m_PathIndex = 0;
	}

	std::optional<NavKeyCommand> Navigator::RecoveryNext()
	{
		// Still waiting for previous back press response
		if (m_PendingStatusMessages > 0)
		{
			return std::nullopt;
		}

		// Check if we're on a blocking page that prevents recovery
		if (IsBlockingPage(m_CurrentPage))
		{
			LogError(Channel::Navigation, [this] { return std::format("Navigator: Cannot recover - stuck on blocking page {}",
				std::to_underlying(m_CurrentPage)); });
			m_State = State::Failed;
			return std::nullopt;
		}

		// Check if we've reached home (System page)
		if (m_CurrentPage == PageId::System)
		{
			LogInfo(Channel::Navigation, "Navigator: Recovery successful, reached home page");
			m_State = State::Navigating;
			m_RecoveryBackPresses = 0;
			m_Path.clear();
			m_PathIndex = 0;
			// Now recompute path from home
			return ExecuteNextStep();
		}

		// Check for pages that don't support Back (OneTouch-style pages)
		if (const MenuPage* current_page = m_Model.GetPage(m_CurrentPage); current_page && !current_page->SupportsKey(EdgeTrigger::Back))
		{
			// This page doesn't support Back - try to navigate to System via selection
			LogDebug(Channel::Navigation, [this, &current_page] { return std::format("Navigator: Page {}({}) doesn't support Back - looking for System navigation",
				std::to_underlying(m_CurrentPage), current_page->name); });

			// Look for a Select edge that goes to System
			for (const auto& edge : current_page->edges)
			{
				if (edge.trigger == EdgeTrigger::Select && edge.target == PageId::System)
				{
					// Content-based resolution for System link
					uint8_t target_line = edge.trigger_line;
					if (!edge.label.empty())
					{
						auto resolved = FindLineByLabel(edge.label);
						if (resolved.has_value())
						{
							if (resolved.value() != edge.trigger_line)
							{
								LogDebug(Channel::Navigation, [&edge, &resolved] { return std::format("Navigator: Recovery: content-based resolution for '{}' at line {} (model says {})",
									edge.label, resolved.value(), edge.trigger_line); });
							}
							target_line = resolved.value();
						}
					}

					LogInfo(Channel::Navigation, [&target_line] { return std::format("Navigator: Found System link at line {}", target_line); });
					// Navigate to the System item
					m_TargetCursorLine = target_line;
					if (m_CursorLine != target_line)
					{
						m_CurrentEdge = &edge;
						m_CursorMoveCount = 0;
						m_State = State::MovingCursor;
						return MoveCursorToTarget();
					}
					else
					{
						m_PendingStatusMessages = 2;
						return NavKeyCommand::Select;
					}
				}
			}

			LogError(Channel::Navigation, "Navigator: Cannot recover from page without Back support and no System link");
			m_State = State::Failed;
			return std::nullopt;
		}

		// Check if too many back presses
		if (m_RecoveryBackPresses >= MAX_BACK_PRESSES)
		{
			LogWarning(Channel::Navigation, [this] { return std::format("Navigator: Too many back presses ({}) in recovery attempt",
				m_RecoveryBackPresses); });
			m_RecoveryAttempts++;
			m_RecoveryBackPresses = 0;

			if (m_RecoveryAttempts >= MAX_RECOVERY_ATTEMPTS)
			{
				LogError(Channel::Navigation, "Navigator: Max recovery attempts exceeded - entering Failed state");
				m_State = State::Failed;
				return std::nullopt;
			}
		}

		// Press back
		m_RecoveryBackPresses++;
		LogDebug(Channel::Navigation, [this] { return std::format("Navigator: Recovery back press {}/{}",
			m_RecoveryBackPresses, MAX_BACK_PRESSES); });

		m_PendingStatusMessages = 2; // Back causes page transition
		return NavKeyCommand::Back;
	}

	std::optional<uint8_t> Navigator::FindLineByLabel(const std::string& label) const
	{
		if (!m_pCurrentContent || label.empty())
		{
			return std::nullopt;
		}

		for (size_t i = 0; i < m_pCurrentContent->Size(); ++i)
		{
			const auto& row = (*m_pCurrentContent)[i];
			if (row.Text.empty())
			{
				continue;
			}

			// Trim leading and trailing whitespace without copying the row text: take a
			// string_view over the original [start, end] range.
			std::string_view trimmed{ row.Text };
			const auto start = trimmed.find_first_not_of(" \t");
			if (start == std::string_view::npos)
			{
				continue;
			}
			const auto end = trimmed.find_last_not_of(" \t");
			trimmed = trimmed.substr(start, end - start + 1);

			// Case-insensitive prefix match
			bool prefix_match = (trimmed.size() >= label.size());
			if (prefix_match)
			{
				for (size_t ci = 0; ci < label.size(); ++ci)
				{
					if (std::tolower(static_cast<unsigned char>(trimmed[ci])) != std::tolower(static_cast<unsigned char>(label[ci])))
					{
						prefix_match = false;
						break;
					}
				}
			}
			if (prefix_match)
			{
				// Word-boundary check: ensure the label isn't a prefix of a longer word
				// e.g., "Help" should match "Help           >" but NOT "HelpChoose..."
				if (trimmed.size() > label.size())
				{
					char next_char = trimmed[label.size()];
					if (std::isalnum(static_cast<unsigned char>(next_char)))
					{
						continue;  // Skip: label is prefix of a longer word
					}
				}

				LogTrace(Channel::Navigation, [&label, &i, &trimmed] { return std::format("Navigator: FindLineByLabel('{}') matched line {} text '{}'",
					label, i, trimmed); });
				return static_cast<uint8_t>(i);
			}
		}

		LogTrace(Channel::Navigation, [&label] { return std::format("Navigator: FindLineByLabel('{}') no match found", label); });
		return std::nullopt;
	}

	std::optional<NavKeyCommand> Navigator::HandlePasswordEntry()
	{
		// Automated PIN entry is not supported (see the password-prompt branch in OnPageUpdate):
		// correctly setting each digit requires reading the current on-screen digit value to
		// decide how many LineUp/LineDown presses to send, which has not been decoded. The old
		// placeholder blindly pressed Select per digit and reported success without ever setting
		// a value, and logged the PIN. We never enter EnteringPassword now, but should this be
		// reached defensively, back out honestly. The PIN value is deliberately never logged.
		LogError(Channel::Navigation, "Navigator: Automated password entry is not supported - backing out of password prompt");
		m_PasswordDigitIndex = 0;
		m_PendingStatusMessages = 2;
		m_State = State::WaitingForPage;
		return NavKeyCommand::Back;
	}

	bool Navigator::HandleSpecialPage(PageId page)
	{
		// Handle special pages that block navigation
		switch (page)
		{
		case PageId::Service:
			LogError(Channel::Navigation, "Navigator: Device is in Service Mode - cannot navigate");
			m_State = State::Failed;
			return false;

		case PageId::TimeOut:
			LogWarning(Channel::Navigation, "Navigator: Device is in Timeout Mode - waiting for user interaction");
			// Timeout mode usually clears with any key press, try Select
			m_PendingStatusMessages = 2;
			// Don't change state - let user handle this or it might clear on its own
			return false;

		default:
			return true;
		}
	}

	bool Navigator::IsBlockingPage(PageId page) const
	{
		return page == PageId::Service || page == PageId::TimeOut;
	}

	bool Navigator::IsTransientPage(PageId page) const
	{
		const MenuPage* p = m_Model.GetPage(page);
		return (p != nullptr) && p->transient;
	}

}
// namespace AqualinkAutomate::Navigation
