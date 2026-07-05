#include "devices/iaq/iaq_page_registry.h"

namespace AqualinkAutomate::Devices::IAQ
{

	PageRegistry DefaultAqualinkTouchRegistry()
	{
		return PageRegistry{
			{ "Pool Heat", { 0x02 }, "pool heat setpoint" },
			{ "Spa Heat",  { 0x03 }, "spa heat setpoint" },
		};
	}

	std::vector<std::uint8_t> BuildSurveyCommandSequence(const PageRegistry& registry, std::uint8_t dwell_polls)
	{
		std::vector<std::uint8_t> commands;

		for (const auto& target : registry)
		{
			// Navigate in: press each button on the path from home to the target page.
			for (auto button_index : target.button_path)
			{
				commands.push_back(static_cast<std::uint8_t>(SURVEY_BUTTON_BASE + button_index));
			}

			// Dwell so the master fully renders the page and the device decodes its data.
			for (std::uint8_t i = 0; i < dwell_polls; ++i)
			{
				commands.push_back(SURVEY_DWELL);
			}

			// Navigate back out to the home page (one Back per level descended).
			for ([[maybe_unused]] const auto& level : target.button_path)
			{
				commands.push_back(SURVEY_BACK);
			}

			// Dwell so the home page settles before visiting the next target.
			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(dwell_polls / 2 + 1); ++i)
			{
				commands.push_back(SURVEY_DWELL);
			}
		}

		return commands;
	}

}
// namespace AqualinkAutomate::Devices::IAQ
