#include "devices/capabilities/scrapeable.h"

#include <magic_enum/magic_enum.hpp>

namespace AqualinkAutomate::Devices::Capabilities
{

	void Scrapeable::ScrapingStart(ScrapeId scrape_graph_id, const uint32_t starting_index)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Scrapeable::ScrapingStart", std::source_location::current());

		if (auto graph_map_it = m_ScraperGraphs.find(scrape_graph_id); m_ScraperGraphs.end() == graph_map_it)
		{
			LogDebug(Channel::Devices, std::format("Scrape -> cannot start; graph id {} does not exist", scrape_graph_id));
		}
		else
		{
			LogDebug(Channel::Devices, std::format("Scrape -> starting active scrape of graph id {}", scrape_graph_id));
			m_ActiveScrape.emplace(scrape_graph_id, ScraperIter::begin(m_ScraperGraphs.at(scrape_graph_id), starting_index));
		}
	}

	void Scrapeable::OnStatusMessageReceived()
	{
		// Pop Status from wait stack if we're expecting one.
		// This must be called BEFORE ScrapingNextWithValidation to ensure
		// the wait stack is correctly updated before processing.
		if (!m_ActiveScrape.has_value() && m_ScrapeState != ScrapeState::RecoveryInProgress)
		{
			// No active scrape and not in recovery - nothing to do
			return;
		}

		if (m_Stack_WaitingForMessage.empty())
		{
			LogTrace(Channel::Devices, "OnStatusMessageReceived: no messages being waited upon");
			return;
		}

		if (m_Stack_WaitingForMessage.top() != Messages::JandyMessageIds::Status)
		{
			LogTrace(Channel::Devices, std::format("OnStatusMessageReceived: waiting for {} not Status",
				magic_enum::enum_name(m_Stack_WaitingForMessage.top())));
			return;
		}

		LogTrace(Channel::Devices, std::format("OnStatusMessageReceived: popping Status, {} remaining",
			m_Stack_WaitingForMessage.size() - 1));
		m_Stack_WaitingForMessage.pop();
	}

	std::expected<Scrapeable::KeyCommand, ErrorCodes::Scrapeable_ErrorCodes> Scrapeable::ScrapingNext()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Scrapeable::ScrapingNext", std::source_location::current());

		if (!m_Stack_WaitingForPage.empty())
		{
			LogTrace(Channel::Devices, "Scrape -> is active; cannot step forward - waiting for screen page");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::WaitingForPage);
		}
		if (!m_Stack_WaitingForMessage.empty())
		{
			LogTrace(Channel::Devices, "Scrape -> is active; cannot step forward - waiting for message");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::WaitingForMessage);
		}
		else if (!m_ActiveScrape.has_value())
		{
			LogTrace(Channel::Devices, "Scrape -> is not active; cannot step forward");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoGraphBeingScraped);
		}
		else
		{
			try
			{
				auto& active_scrape = m_ActiveScrape.value();
				auto& id = std::get<ScrapeId>(active_scrape);
				auto& it = std::get<ScraperIter>(active_scrape);

					// Check if at end before advancing, then check again after advancing
				bool at_end = (ScraperIter::end(m_ScraperGraphs.at(id)) == it);
				if (!at_end)
				{
					++it;
					at_end = (ScraperIter::end(m_ScraperGraphs.at(id)) == it);
				}
				if (at_end)
				{
					LogTrace(Channel::Devices, "Scrape -> is complete");
					m_ActiveScrape = std::nullopt;
					return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoStepPossible);
				}
				else if (auto key_command = std::get<Utility::ScreenDataPageGraphImpl::Edge>(*it).key_command; KeyCommand::NoKeyCommand == key_command)
				{
					LogDebug(Channel::Devices, "Attempted to retrieve the next key command; key command had no value");
					return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::UnknownScrapeError);
				}
				else
				{
					LogTrace(Channel::Devices, "Scrape -> is active; making next step");

					// Wait for 1 Status message (cursor movements only send 1)
					m_Stack_WaitingForMessage.push(Messages::JandyMessageIds::Status);

					return key_command;
				}
			}
			catch (const std::bad_optional_access& eBOA)
			{
				LogTrace(Channel::Devices, std::format("Scrape -> was active but could not access graph (exception was -> {})", eBOA.what()));
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoGraphBeingScraped);
			}
		}
	}

	std::expected<Scrapeable::KeyCommand, ErrorCodes::Scrapeable_ErrorCodes>
		Scrapeable::ScrapingNextWithValidation(Utility::ScreenDataPageTypes current_page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Scrapeable::ScrapingNextWithValidation", std::source_location::current());

		// 1. If Faulted -> return MaxRecoveryAttemptsExceeded
		if (ScrapeState::Faulted == m_ScrapeState)
		{
			LogError(Channel::Scraping, "Scraping is in Faulted state - cannot proceed");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::MaxRecoveryAttemptsExceeded);
		}

		// 2. If RecoveryInProgress -> delegate to RecoveryNext
		if (ScrapeState::RecoveryInProgress == m_ScrapeState)
		{
			LogDebug(Channel::Scraping, std::format("Recovery in progress, current_page={}", magic_enum::enum_name(current_page)));
			return RecoveryNext(current_page);
		}

		// 3. Check if waiting for messages BEFORE post-validation
		// For page transitions, we wait for 2 Status messages. We must wait for ALL messages
		// before validating, otherwise we'd validate after the first Status (which just
		// acknowledges the command) before the page has actually changed.
		if (!m_Stack_WaitingForMessage.empty())
		{
			LogTrace(Channel::Scraping, std::format("Scrape -> waiting for {} more message(s)", m_Stack_WaitingForMessage.size()));
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::WaitingForMessage);
		}

		// 4. If AwaitingPostValidation -> validate we arrived at expected destination
		// This only runs after ALL Status messages have been received.
		if (ScrapeState::AwaitingPostValidation == m_ScrapeState)
		{
			if (!ValidatePostCommand(current_page))
			{
				LogError(Channel::Scraping, std::format("Post-command validation failed: expected={}, actual={}",
					magic_enum::enum_name(m_ExpectedDestination), magic_enum::enum_name(current_page)));
				InitiateRecovery();
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::PostCommandValidationFailed);
			}
			LogDebug(Channel::Scraping, std::format("Post-command validation passed: page={}", magic_enum::enum_name(current_page)));
			m_ScrapeState = ScrapeState::Idle;
		}

		// 5. Check if there's an active scrape
		if (!m_ActiveScrape.has_value())
		{
			LogTrace(Channel::Scraping, "Scrape -> is not active; cannot step forward");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoGraphBeingScraped);
		}

		try
		{
			auto& active_scrape = m_ActiveScrape.value();
			auto& id = std::get<ScrapeId>(active_scrape);
			auto& it = std::get<ScraperIter>(active_scrape);

			// Check if we've reached the end
			if (ScraperIter::end(m_ScraperGraphs.at(id)) == it)
			{
				LogInfo(Channel::Scraping, "Scrape -> is complete");
				m_ActiveScrape = std::nullopt;
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoStepPossible);
			}

			// 6. Get current vertex's expected page (the source vertex)
			auto current_vertex = std::get<Utility::ScreenDataPageGraphImpl::Vertex>(*it);
			m_ExpectedSource = current_vertex.page;

			// Move to next step
			if (++it; ScraperIter::end(m_ScraperGraphs.at(id)) == it)
			{
				LogInfo(Channel::Scraping, "Scrape -> is complete");
				m_ActiveScrape = std::nullopt;
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoStepPossible);
			}

			// Get the edge's key command and the target vertex
			auto edge = std::get<Utility::ScreenDataPageGraphImpl::Edge>(*it);
			auto target_vertex = std::get<Utility::ScreenDataPageGraphImpl::Vertex>(*it);

			if (KeyCommand::NoKeyCommand == edge.key_command)
			{
				LogDebug(Channel::Scraping, "Attempted to retrieve the next key command; key command had no value");
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::UnknownScrapeError);
			}

			// 7. PRE-COMMAND VALIDATION
			if (!ValidatePreCommand(current_page))
			{
				LogError(Channel::Scraping, std::format("Pre-command validation failed: expected={}, actual={}",
					magic_enum::enum_name(m_ExpectedSource), magic_enum::enum_name(current_page)));
				InitiateRecovery();
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::PreCommandValidationFailed);
			}
			LogDebug(Channel::Scraping, std::format("Pre-command validation passed: page={}", magic_enum::enum_name(current_page)));

			// 8. Extract expected destination from target vertex
			m_ExpectedDestination = target_vertex.page;
			m_ScrapeState = ScrapeState::AwaitingPostValidation;

			LogDebug(Channel::Scraping, std::format("Sending command, expecting destination={}",
				magic_enum::enum_name(m_ExpectedDestination)));

			// 9. Push Status messages onto wait stack
			// For cursor movements (same page), controller sends 1 Status message.
			// For page transitions (different page), controller sends:
			//   - Status #1: Acknowledges the command
			//   - Clear message + MessageLong messages for new page content
			//   - Status #2: Signals end of page update
			// We must wait for the second Status to validate the new page type.
			//
			// Page_Unknown handling:
			// - If both source and destination are Page_Unknown: assume cursor movement (1 Status)
			//   because we're likely staying on the same undetectable page type
			// - If source and destination are the same known page: cursor movement (1 Status)
			// - Otherwise (one known, one unknown, or different known pages): assume page
			//   transition (2 Status) to be safe - we can't tell if the page will change
			bool is_cursor_movement =
				(m_ExpectedSource == m_ExpectedDestination) ||
				(m_ExpectedSource == Utility::ScreenDataPageTypes::Page_Unknown &&
				 m_ExpectedDestination == Utility::ScreenDataPageTypes::Page_Unknown);
			if (bool is_page_transition = !is_cursor_movement)
			{
				LogDebug(Channel::Scraping, std::format("Page transition: {} -> {}, waiting for 2 Status messages",
					magic_enum::enum_name(m_ExpectedSource), magic_enum::enum_name(m_ExpectedDestination)));
				m_Stack_WaitingForMessage.push(Messages::JandyMessageIds::Status);
				m_Stack_WaitingForMessage.push(Messages::JandyMessageIds::Status);
			}
			else
			{
				LogDebug(Channel::Scraping, std::format("Cursor movement on {}, waiting for 1 Status message",
					magic_enum::enum_name(m_ExpectedSource)));
				m_Stack_WaitingForMessage.push(Messages::JandyMessageIds::Status);
			}

			// 10. Return key_command
			return edge.key_command;
		}
		catch (const std::bad_optional_access& eBOA)
		{
			LogTrace(Channel::Scraping, std::format("Scrape -> was active but could not access graph (exception was -> {})", eBOA.what()));
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::NoGraphBeingScraped);
		}
	}

	bool Scrapeable::ValidatePreCommand(Utility::ScreenDataPageTypes current_page) const
	{
		// If expected source is Page_Unknown, accept any page (wildcard)
		if (Utility::ScreenDataPageTypes::Page_Unknown == m_ExpectedSource)
		{
			return true;
		}
		return current_page == m_ExpectedSource;
	}

	bool Scrapeable::ValidatePostCommand(Utility::ScreenDataPageTypes current_page)
	{
		// If expected destination is Page_Unknown, accept any page (wildcard)
		if (Utility::ScreenDataPageTypes::Page_Unknown == m_ExpectedDestination)
		{
			return true;
		}
		return current_page == m_ExpectedDestination;
	}

	void Scrapeable::InitiateRecovery()
	{
		m_RecoveryAttempts++;
		LogWarning(Channel::Scraping, std::format("Initiating recovery, attempt {}/{}",
			m_RecoveryAttempts, MAX_RECOVERY_ATTEMPTS));

		if (m_RecoveryAttempts > MAX_RECOVERY_ATTEMPTS)
		{
			LogError(Channel::Scraping, std::format("Max recovery attempts ({}) exceeded - entering Faulted state", MAX_RECOVERY_ATTEMPTS));
			m_ScrapeState = ScrapeState::Faulted;
			return;
		}

		m_ScrapeState = ScrapeState::RecoveryInProgress;
		m_RecoveryBackPresses = 0;
	}

	std::expected<Scrapeable::KeyCommand, ErrorCodes::Scrapeable_ErrorCodes>
		Scrapeable::RecoveryNext(Utility::ScreenDataPageTypes current_page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Scrapeable::RecoveryNext", std::source_location::current());

		// Check if we're waiting for messages from a previous Back press
		if (!m_Stack_WaitingForMessage.empty())
		{
			LogTrace(Channel::Scraping, "Recovery -> waiting for message response");
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::WaitingForMessage);
		}

		// 1. If we've reached Home, recovery is complete
		if (Utility::ScreenDataPageTypes::Page_System == current_page)
		{
			LogInfo(Channel::Scraping, "Recovery successful, reached Home screen");
			m_ScrapeState = ScrapeState::Idle;
			m_RecoveryBackPresses = 0;
			return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::RecoveryComplete);
		}

		// 2. If too many back presses in this attempt, increment attempt counter
		if (m_RecoveryBackPresses >= MAX_BACK_PRESSES)
		{
			LogWarning(Channel::Scraping, std::format("Too many back presses ({}) in recovery attempt", m_RecoveryBackPresses));
			m_RecoveryAttempts++;
			m_RecoveryBackPresses = 0;

			if (m_RecoveryAttempts >= MAX_RECOVERY_ATTEMPTS)
			{
				LogError(Channel::Scraping, std::format("Max recovery attempts ({}) exceeded - entering Faulted state", MAX_RECOVERY_ATTEMPTS));
				m_ScrapeState = ScrapeState::Faulted;
				return std::unexpected(ErrorCodes::Scrapeable_ErrorCodes::MaxRecoveryAttemptsExceeded);
			}
		}

		// 3. Press Back
		m_RecoveryBackPresses++;
		LogDebug(Channel::Scraping, std::format("Recovery step: pressing Back ({}/{}), current_page={}",
			m_RecoveryBackPresses, MAX_BACK_PRESSES, magic_enum::enum_name(current_page)));

		// Wait for 1 Status message after Back press
		m_Stack_WaitingForMessage.push(Messages::JandyMessageIds::Status);

		// Return the device-specific recovery (Back) key command
		return m_RecoveryKeyCommand;
	}

	Scrapeable::ScrapeState Scrapeable::GetScrapeState() const
	{
		return m_ScrapeState;
	}

	void Scrapeable::ResetRecoveryState()
	{
		LogInfo(Channel::Scraping, "Resetting recovery state for restart");
		m_ScrapeState = ScrapeState::Idle;
		// Note: m_RecoveryAttempts is NOT reset here - it persists across recovery cycles
		// until scraping completes successfully. Use ResetAllScrapingState() for full reset.
		m_RecoveryBackPresses = 0;
		m_ExpectedDestination = Utility::ScreenDataPageTypes::Page_Unknown;
		m_ExpectedSource = Utility::ScreenDataPageTypes::Page_Unknown;
	}

	void Scrapeable::ResetAllScrapingState()
	{
		LogInfo(Channel::Scraping, "Resetting all scraping state (successful completion)");
		m_ScrapeState = ScrapeState::Idle;
		m_RecoveryAttempts = 0;
		m_RecoveryBackPresses = 0;
		m_ExpectedDestination = Utility::ScreenDataPageTypes::Page_Unknown;
		m_ExpectedSource = Utility::ScreenDataPageTypes::Page_Unknown;
	}

}
// namespace AqualinkAutomate::Devices::Capabilities
