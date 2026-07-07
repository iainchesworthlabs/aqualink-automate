#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <stack>
#include <tuple>
#include <unordered_map>

#include <magic_enum/magic_enum.hpp>

#include "devices/jandy_device_types.h"
#include "errors/jandy_errors_scrapeable.h"
#include "formatters/jandy_device_formatters.h"
#include "logging/logging.h"
#include "messages/jandy_message_ids.h"
#include "profiling/profiling.h"
#include "utility/screen_data_page_graph.h"
#include "utility/screen_data_page_graph/screen_data_page_graph_traverse.h"
#include "utility/screen_data_page_processor.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices::Capabilities
{

	// Scrapeable is the GRAPH-DRIVEN screen-scraping engine: a device supplies one or
	// more pre-computed ScreenDataPageGraphs (vertex = page, edge = key command) and the
	// engine walks them deterministically, validating each transition.  It is used by the
	// PDA device (Capabilities::Scrapeable).
	//
	// A second, separate engine lives under src/jandy/navigation (Navigator + SpiderEngine
	// + MenuModel).  That one is DETECTOR/MODEL-DRIVEN: it discovers the menu structure at
	// runtime from page-title detectors and computes paths on the fly, which the fixed graph
	// here cannot do.  The two intentionally coexist for now because they solve different
	// problems (static known-route scraping vs. autonomous menu crawling) and serve different
	// devices (PDA vs. OneTouch/iAQ).  They DO duplicate the recovery / back-press /
	// home-detection state machine (note the matching MAX_RECOVERY_ATTEMPTS / MAX_BACK_PRESSES
	// constants in both Navigator and Scrapeable).  Folding Scrapeable into Navigator (a full
	// migration) is deferred — see WU-NAV-SCREENDATA-GRAPH — because it touches the navigation
	// subsystem owned by a separate work unit and changes PDA behaviour.
	class Scrapeable
	{
	public:
		enum class ScrapeState
		{
			Idle,                    // Ready to scrape
			AwaitingPostValidation,  // Command sent, waiting to validate destination
			RecoveryInProgress,      // Pressing Back to reach Home
			Faulted                  // Unrecoverable error
		};

		using ScrapeId = uint32_t;
		using ScraperGraph = Utility::ScreenDataPageGraph;
		using ScraperIter = Utility::ScreenDataPageGraphImpl::ForwardIterator;
		using KeyCommand = Utility::ScreenDataPageGraphImpl::KeyCommand;

		static constexpr uint32_t MAX_RECOVERY_ATTEMPTS = 3;
		static constexpr uint32_t MAX_BACK_PRESSES = 10;

	public:
		// Set the key command to use during recovery (should be the "Back" key for the device)
		void SetRecoveryKeyCommand(KeyCommand recovery_key) { m_RecoveryKeyCommand = recovery_key; }

	public:
		using GraphDataMap = std::unordered_map<ScrapeId, ScraperGraph>;
		using GraphIterMap = std::unordered_map<ScrapeId, ScraperIter>;

	public:
		template<typename... MESSAGE_TYPES>
		Scrapeable(const Devices::JandyDeviceType device_id, GraphDataMap graphs, MESSAGE_TYPES ...) :
			m_ScraperGraphs(graphs),
			m_ParentDeviceId(device_id)
		{
			// Note: Message handling is now done via explicit OnStatusMessageReceived() calls
			// from the device's message handlers. This ensures proper ordering: the wait stack
			// is updated BEFORE ScrapingNextWithValidation checks it.
			// The previous lambda-based SignalBus approach had race conditions where the lambda
			// might pop messages before the current command's push, or run at unpredictable times.
		}

		void ScrapingStart(ScrapeId scrape_graph_id, const uint32_t starting_index = 1);

		// Call this when a Status message is received to pop from the wait stack.
		// This should be called BEFORE ScrapingNextWithValidation to ensure
		// the wait stack is updated before processing.
		void OnStatusMessageReceived();

		std::expected<KeyCommand, ErrorCodes::Scrapeable_ErrorCodes> ScrapingNext();

		// Main validation-aware scraping method
		std::expected<KeyCommand, ErrorCodes::Scrapeable_ErrorCodes>
			ScrapingNextWithValidation(Utility::ScreenDataPageTypes current_page);

		// Validation helpers
		bool ValidatePreCommand(Utility::ScreenDataPageTypes current_page) const;
		bool ValidatePostCommand(Utility::ScreenDataPageTypes current_page);

		// Recovery
		void InitiateRecovery();
		std::expected<KeyCommand, ErrorCodes::Scrapeable_ErrorCodes>
			RecoveryNext(Utility::ScreenDataPageTypes current_page);

		// State accessors
		ScrapeState GetScrapeState() const;
		void ResetRecoveryState();      // Resets per-recovery state (for restart after recovery)
		void ResetAllScrapingState();   // Resets all state including attempt counter (for successful completion)

	private:
		GraphDataMap m_ScraperGraphs;
		std::optional<std::tuple<ScrapeId, ScraperIter>> m_ActiveScrape{ std::nullopt };

		std::stack<Utility::ScreenDataPageTypes> m_Stack_WaitingForPage;
		std::stack<Messages::JandyMessageIds> m_Stack_WaitingForMessage;
		const Devices::JandyDeviceType m_ParentDeviceId;

		ScrapeState m_ScrapeState{ ScrapeState::Idle };
		uint32_t m_RecoveryAttempts{ 0 };
		uint32_t m_RecoveryBackPresses{ 0 };
		Utility::ScreenDataPageTypes m_ExpectedDestination{ Utility::ScreenDataPageTypes::Page_Unknown };
		Utility::ScreenDataPageTypes m_ExpectedSource{ Utility::ScreenDataPageTypes::Page_Unknown };
		KeyCommand m_RecoveryKeyCommand{ KeyCommand::NoKeyCommand };  // Device-specific "Back" key command
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
