#pragma once

#include <format>
#include <ostream>
#include <string>
#include <string_view>

#include <boost/beast/core/string.hpp>


namespace AqualinkAutomate
{

	auto operator<<(std::ostream& os, const boost::beast::string_view& obj) -> std::ostream&;

}
// namespace AqualinkAutomate

template<>
struct std::formatter<boost::beast::string_view>
{
	std::formatter<std::string_view> m_Base;

	constexpr auto parse(std::format_parse_context& ctx)
	{
		return m_Base.parse(ctx);
	}

	template<typename FormatContext>
	auto format(const boost::beast::string_view& sv, FormatContext& ctx) const
	{
		return m_Base.format(std::string_view{ sv.data(), sv.size() }, ctx);
	}
};
