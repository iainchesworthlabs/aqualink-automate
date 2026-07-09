#pragma once

#include <format>
#include <ostream>
#include <string>
#include <string_view>

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>


namespace std
{

	std::ostream& operator<<(std::ostream& os, const boost::asio::ip::tcp::endpoint& obj);

}
// namespace std

template<>
struct std::formatter<boost::asio::ip::tcp::endpoint>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const boost::asio::ip::tcp::endpoint& endpoint, FormatContext& ctx) const
	{
		const auto extract_address_as_string = [](const auto& endpoint) -> std::string
			{
				if (auto addr = endpoint.address(); addr.is_v6())
				{
					if (auto ipv6 = addr.to_v6(); ipv6.is_v4_mapped())
					{
						auto ipv4{ boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ipv6) };
						return ipv4.to_string();
					}
					else
					{
						return ipv6.to_string();
					}
				}
				else
				{
					return addr.to_string();
				}
			};

		const auto host{ extract_address_as_string(endpoint) };
		const auto port{ endpoint.port() };

		return std::format_to(ctx.out(), "{}:{}", host, port);
	}
};
