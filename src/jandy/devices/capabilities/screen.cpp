#include <format>
#include <ranges>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "devices/capabilities/screen.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices::Capabilities
{

	Screen::Screen(uint8_t screen_lines) :
		m_DisplayedPage(screen_lines),
		m_DisplayedPageUpdater(m_DisplayedPage)
	{
		m_DisplayedPageUpdater.initiate();
	}

	void Screen::ProcessScreenUpdates()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Screen::ProcessScreenUpdates", std::source_location::current());

		switch (m_ScreenMode)
		{
		case ScreenModes::Normal:
		{
			auto normal_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Screen::ProcessScreenUpdates -> normal_mode", std::source_location::current());
			LogTrace(Channel::Devices, "Device screen mode -> normal");
			break;
		}

		case ScreenModes::Updating:
		{
			auto updating_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Screen::ProcessScreenUpdates -> updating_mode", std::source_location::current());
			LogTrace(Channel::Devices, "Device screen mode -> updating");
			break;
		}

		case ScreenModes::UpdateComplete:
		{
			auto update_complete_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Screen::ProcessScreenUpdates -> update_complete", std::source_location::current());
			LogTrace(Channel::Devices, "Device screen mode -> update complete");

			// Set the current page to "unknown"; if there's a page processor, we'll set the page to that later...
			m_DisplayedPageType = Utility::ScreenDataPageTypes::Page_Unknown;

			// Process the "page" to extract information.
			auto actionable_processors = m_DisplayedPageProcessors | std::views::filter([this](const decltype(m_DisplayedPageProcessors)::value_type& processor) { return processor.CanProcess(m_DisplayedPage); });
			for (auto& processor : actionable_processors)
			{
				auto process_page_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Screen::ProcessScreenUpdates -> process_page", std::source_location::current());
				LogTrace(Channel::Devices, std::format("Device screen mode -> processing page {}", magic_enum::enum_name(processor.PageType())));

				// As there's a specific processor, set the page type to the processor's page type.
				m_DisplayedPageType = processor.PageType();

				// Process the page.
				processor.Process(m_DisplayedPage);
			}

			m_ScreenMode = ScreenModes::Normal;
			break;
		}
		}
	}

	ScreenModes Screen::ScreenMode() const
	{
		return m_ScreenMode;
	}

	void Screen::ScreenMode(ScreenModes screen_mode)
	{
		m_ScreenMode = screen_mode;
	}

	std::list<Utility::ScreenDataPage_Processor> const& Screen::PageProcessors() const
	{
		return m_DisplayedPageProcessors;
	}

	void Screen::PageProcessors(std::list<Utility::ScreenDataPage_Processor>&& page_processors)
	{
		m_DisplayedPageProcessors = std::move(page_processors);
	}

	Utility::ScreenDataPage const& Screen::DisplayedPage() const
	{
		return m_DisplayedPage;
	}

	Utility::ScreenDataPageTypes Screen::DisplayedPageType() const
	{
		return m_DisplayedPageType;
	}

	void Screen::DisplayedPageType(Utility::ScreenDataPageTypes page_type)
	{
		m_DisplayedPageType = page_type;
	}

	nlohmann::json Screen::DescribeScreen() const
	{
		nlohmann::json screen;

		screen["page_type"] = std::string(magic_enum::enum_name(DisplayedPageType()));
		screen["mode"] = std::string(magic_enum::enum_name(ScreenMode()));

		nlohmann::json lines = nlohmann::json::array();
		const auto& page = DisplayedPage();
		for (std::size_t i = 0; i < page.Size(); ++i)
		{
			lines.push_back(page[i].Text);
		}
		screen["lines"] = lines;

		return screen;
	}

}
// namespace AqualinkAutomate::Devices::Capabilities
