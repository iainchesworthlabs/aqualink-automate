#pragma once

#include <format>
#include <ostream>
#include <string>

#include "utility/signalling_stats_counter.h"

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Utility::StatsCounter& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Utility::StatsCounter>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Utility::StatsCounter& stats_counter, FormatContext& ctx) const
	{
		const auto v{ stats_counter.Count() };
		return std::format_to(ctx.out(), "{}", v);
	}
};
