#pragma once

#include <format>
#include <ostream>
#include <string>

#include "devices/jandy_device_id.h"
#include "devices/jandy_device_types.h"

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Devices::JandyDeviceId& obj);
	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Devices::JandyDeviceType& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Devices::JandyDeviceId>
{
	constexpr auto parse(std::format_parse_context& ctx) 
	{ 
		return ctx.begin(); 
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Devices::JandyDeviceId& device_id, FormatContext& ctx) const -> typename FormatContext::iterator
	{
		const auto v{ device_id() };
		return std::format_to(ctx.out(), "0x{:02x}", v);
	}
};

template<>
struct std::formatter<AqualinkAutomate::Devices::JandyDeviceType>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Devices::JandyDeviceType& device_type, FormatContext& ctx) const
	{
		const auto v{ device_type.Id() };
		return std::format_to(ctx.out(), "{}", v);
	}
};
