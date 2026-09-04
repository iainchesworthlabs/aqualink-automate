#pragma once

#include <format>
#include <ostream>
#include <string>
#include <string_view>

#include "kernel/ph.h"


namespace AqualinkAutomate::Kernel
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::pH& obj);

}
// namespace AqualinkAutomate::Kernel

template<>
struct std::formatter<AqualinkAutomate::Kernel::pH>
{
	std::formatter<boost::float32_t> m_Base;

	constexpr auto parse(std::format_parse_context& ctx)
	{
		if (auto it = ctx.begin(); it == ctx.end() || *it == '}')
		{
			std::basic_format_parse_context<char> default_ctx(std::string_view(".01f"));
			m_Base.parse(default_ctx);
			return it;
		}

		return m_Base.parse(ctx);
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Kernel::pH& pH, FormatContext& ctx) const
	{
		return m_Base.format(pH(), ctx);
	}
};
