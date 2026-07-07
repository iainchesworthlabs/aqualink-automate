#pragma once

#include <format>
#include <ostream>
#include <string>

#include "formatters/units_electric_potential_formatter.h"
#include "kernel/orp.h"


namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Kernel::ORP& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Kernel::ORP>
{
	std::formatter<AqualinkAutomate::Units::millivolt_quantity> m_Base;

	constexpr auto parse(std::format_parse_context& ctx)
	{
		return m_Base.parse(ctx);
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Kernel::ORP& orp, FormatContext& ctx) const
	{
		return m_Base.format(orp(), ctx);
	}
};
