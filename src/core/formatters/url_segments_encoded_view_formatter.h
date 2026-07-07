#pragma once

#include <format>
#include <ostream>
#include <iterator>
#include <string>
#include <string_view>

#include <boost/url/segments_encoded_view.hpp>


namespace std
{

	std::ostream& operator<<(std::ostream& os, const boost::urls::segments_encoded_view& obj);

}
// namespace std

template<>
struct std::formatter<boost::urls::segments_encoded_view>
{
	constexpr auto parse(format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const boost::urls::segments_encoded_view& sev, FormatContext& ctx) const
	{
		auto out = ctx.out();

		for (const auto& sev_elem : sev)
		{
			out = std::format_to(out, "/{}", std::string_view{ sev_elem.data(), sev_elem.size() });
		}

		return out;
	}
};
