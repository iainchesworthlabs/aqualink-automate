#pragma once

#include <format>
#include <string>
#include <string_view>

#include "utility/screen_data_page.h"

template<>
struct std::formatter<AqualinkAutomate::Utility::ScreenDataPage>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Utility::ScreenDataPage& page, FormatContext& ctx) const
	{
		using AqualinkAutomate::Utility::ScreenDataPage::HighlightStates::Highlighted;

		static const std::string_view HEADER_FOOTER{ "+------------------+\n" };
		static const std::string_view PAGE_ROW{ "| {:^16} | {}\n" };

		auto out = ctx.out();

		out = std::copy(HEADER_FOOTER.begin(), HEADER_FOOTER.end(), out);

		for (const auto& row : page.m_Rows)
		{
			const auto highlight_arrow = (Highlighted == row.HighlightState) ? "<--" : "";
			out = std::vformat_to(out, PAGE_ROW, std::make_format_args(row.Text, highlight_arrow));
		}

		out = std::copy(HEADER_FOOTER.begin(), HEADER_FOOTER.end(), out);

		return out;
	}
};
