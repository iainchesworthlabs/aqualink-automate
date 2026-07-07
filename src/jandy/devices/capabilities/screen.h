#pragma once

#include <cstdint>
#include <list>

#include <nlohmann/json.hpp>

#include "utility/screen_data_page.h"
#include "utility/screen_data_page_processor.h"
#include "utility/screen_data_page_updater.h"

namespace AqualinkAutomate::Devices::Capabilities
{

	enum class ScreenModes
	{
		Normal,
		Updating,
		UpdateComplete
	};

	class Screen
	{
	public:
		explicit Screen(uint8_t screen_lines);

		Utility::ScreenDataPage const& DisplayedPage() const;
		Utility::ScreenDataPageTypes DisplayedPageType() const;

	protected:
		// Directly mark the displayed page as a known, fixed type.  This is for
		// devices whose "screen" is a rendered reflection of decoded status (e.g.
		// the IAQ System Status view) rather than a navigable, processor-matched
		// page.  Devices that scrape navigable pages should continue to let the
		// page processors set the type via ProcessScreenUpdates() instead.
		void DisplayedPageType(Utility::ScreenDataPageTypes page_type);

	public:
		// Serialise the current screen (page type, mode, line text) as JSON
		// for diagnostics consumers — single source of truth for every device
		// that exposes a screen.
		nlohmann::json DescribeScreen() const;

	private:
		Utility::ScreenDataPage m_DisplayedPage;
		Utility::ScreenDataPageTypes m_DisplayedPageType{ Utility::ScreenDataPageTypes::Page_Unknown };

	public:
		template<typename EVENT_TYPE>
		void ProcessScreenEvent(const EVENT_TYPE& event_type)
		{
			m_DisplayedPageUpdater.process_event(event_type);
		}

		void ProcessScreenUpdates();

		ScreenModes ScreenMode() const;
		void ScreenMode(ScreenModes screen_mode);

	private:
		ScreenModes m_ScreenMode{ ScreenModes::Normal };

	public:
		std::list<Utility::ScreenDataPage_Processor> const& PageProcessors() const;
		void PageProcessors(std::list<Utility::ScreenDataPage_Processor>&& page_processors);

	private:
		Utility::ScreenDataPageUpdater<Utility::ScreenDataPage> m_DisplayedPageUpdater;
		std::list<Utility::ScreenDataPage_Processor> m_DisplayedPageProcessors;
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
