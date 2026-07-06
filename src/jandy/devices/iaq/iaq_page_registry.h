#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AqualinkAutomate::Devices::IAQ
{

	// A data page the emulated AqualinkTouch should visit on start-up, reached from the home
	// page by pressing PageButtons in sequence (button indices, as decoded from the master's
	// IAQMessage_PageButton frames). This is the DECLARATIVE alternative to spidering a menu:
	// the home-page status arrives pushed (no navigation), so only the pages carrying data the
	// home page does NOT have are listed here.
	struct PageTarget
	{
		std::string label;                       // e.g. "Pool Heat"
		std::vector<std::uint8_t> button_path;   // home -> target, by PageButton index
		std::string purpose;                     // what data this page sources
	};

	using PageRegistry = std::vector<PageTarget>;

	// The default AqualinkTouch data pages. button_path is best-effort against the observed home
	// layout (0=Filter Pump, 1=Spa, 2=Pool Heat, 3=Spa Heat, 4-6=Aux); the heat pages carry the
	// setpoints the home-page MainStatus push does not. Refine per panel config against captures.
	PageRegistry DefaultAqualinkTouchRegistry();

	// Survey command opcodes -- these ride the 0x33 poll-ACK channel (see IAQDevice::SelectPageButton
	// and the IAQ_CMD_* constants in iaq_device.cpp).
	inline constexpr std::uint8_t SURVEY_DWELL{ 0x00 };        // no button pressed -> let the page render
	inline constexpr std::uint8_t SURVEY_BACK{ 0x02 };         // navigate back one level toward home
	inline constexpr std::uint8_t SURVEY_BUTTON_BASE{ 0x11 };  // press PageButton index N -> 0x11 + N

	// Flatten the registry into a poll-ACK command sequence that visits each target and returns
	// home: for each target -> press its button path, dwell while the master renders the page (so
	// the device decodes it), then Back out one level per descent and dwell before the next. The
	// queue drains one command per IAQ_Poll, so `dwell_polls` paces the survey against rendering.
	std::vector<std::uint8_t> BuildSurveyCommandSequence(const PageRegistry& registry, std::uint8_t dwell_polls = 6);

	// Start-up page-survey state, extracted from IAQDevice (SonarCloud S1820): whether a survey is
	// armed and whether it has already run (it runs once), plus the declarative registry of pages to
	// visit. The device gates the actual launch on being emulated + the home page being established
	// and enqueues the built command sequence; this type only owns the arm/consume decision.
	class PageSurvey
	{
	public:
		// Arm a survey with the pages to visit (the device calls this from EnablePageSurvey).
		void Enable(const PageRegistry& registry) { m_Enabled = true; m_Registry = registry; }

		// Armed and not yet run? (The device additionally requires emulated + home established.)
		bool IsArmed() const { return m_Enabled && !m_Done; }

		std::size_t TargetCount() const { return m_Registry.size(); }

		// Consume the one-shot survey: mark it run and return the poll-ACK command sequence to enqueue.
		std::vector<std::uint8_t> Consume() { m_Done = true; return BuildSurveyCommandSequence(m_Registry); }

	private:
		bool m_Enabled{ false };   // arm a start-up data-page survey (auto-startup page-push)
		bool m_Done{ false };      // the survey runs once, after home is established
		PageRegistry m_Registry;   // the declarative pages to visit
	};

}
// namespace AqualinkAutomate::Devices::IAQ
