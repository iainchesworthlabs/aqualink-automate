#include <algorithm>

#include "utility/endpoint_parser.h"

namespace AqualinkAutomate::Utility
{

    namespace EndpointParserImpl
    {
        // Constexpr-compatible whitespace check
        constexpr bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        constexpr std::string_view ltrim(std::string_view sv) noexcept
        {
            while (!sv.empty() && is_space(sv.front()))
            {
                sv.remove_prefix(1);
            }
            return sv;
        }

        constexpr std::string_view rtrim(std::string_view sv) noexcept
        {
            while (!sv.empty() && is_space(sv.back()))
            {
                sv.remove_suffix(1);
            }
            return sv;
        }

        constexpr std::string_view trim(std::string_view sv) noexcept
        {
            return rtrim(ltrim(sv));
        }

        // Strip optional "<scheme>://"
        constexpr std::string_view strip_scheme(std::string_view sv) noexcept
        {
            // find "://"
            // Only strip if there's at least one character before "://" (pos > 0)
            if (const auto pos = sv.find("://"); pos != std::string_view::npos && pos > 0)
            {
                return sv.substr(pos + 3);
            }
            // If "://" is at position 0 (no scheme), return as-is
            return sv;
        }

        // Decode percent-encoded zone ID
        // For constexpr context, we return the view starting after %25
        constexpr std::string_view decode_zone_id(std::string_view zone) noexcept
        {
            // Check if it starts with "25" (percent-encoded %)
            if (zone.size() >= 2 && zone[0] == '2' && zone[1] == '5')
            {
                return zone.substr(2);
            }
            return zone;
        }

        constexpr std::optional<std::uint16_t> parse_port_number(std::string_view sv) noexcept
        {
            if (sv.empty() || sv.size() > 5)
            {
                return std::nullopt;
            }

            std::uint32_t v = 0;
            for (char c : sv)
            {
                if (c < '0' || c > '9')
                {
                    return std::nullopt;
                }
                v = v * 10u + static_cast<std::uint32_t>(c - '0');
                if (v > 65535u)
                {
                    return std::nullopt;
                }
            }

            // Port 0 is not valid for network connections
            if (v == 0u)
            {
                return std::nullopt;
            }

            return static_cast<std::uint16_t>(v);
        }

        std::tuple<std::string, std::string> materialize_host_port(const EndpointView& v)
        {
            return { std::string(v.host), std::string(v.port) };
        }
    }
    // namespace EndpointParserImpl

    EndpointView ParseEndpointView(std::string_view input, std::string_view default_port)
    {
        using namespace EndpointParserImpl;

        EndpointView out{};
        input = trim(input);

        if (input.empty())
        {
            if (!default_port.empty())
            {
                out.port = default_port;
                out.port_num = parse_port_number(default_port);
            }
            return out;
        }

        input = strip_scheme(input);

        if (!input.empty() && input.front() == '[')
        {
            // [IPv6[%zone]]:port
            out.had_brackets = true;
            const auto rb = input.find(']');

            if (rb == std::string_view::npos)
            {
                // Malformed: treat everything after '[' as host
                out.host = input.substr(1);
            }
            else
            {
                auto inner = input.substr(1, rb - 1);

                // RFC 6874: zone id after '%'
                if (const auto pct = inner.find('%'); pct != std::string_view::npos)
                {
                    out.scope_id = decode_zone_id(inner.substr(pct + 1));
                    inner = inner.substr(0, pct);
                }
                out.host = inner;

                if (rb + 1 < input.size() && input[rb + 1] == ':')
                {
                    out.port = input.substr(rb + 2);
                }
            }
        }
        else
        {
            // hostname or IPv4 (we will NOT split unbracketed IPv6)
            const auto last_colon = input.rfind(':');
            bool split = false;

            if (last_colon != std::string_view::npos)
            {
                auto host_part = input.substr(0, last_colon);

                // Only split if there are no additional colons in host_part
                // If multiple colons exist, treat entire input as unbracketed IPv6
                // Users MUST use brackets for IPv6 with port: [ipv6]:port
                if (host_part.find(':') == std::string_view::npos)
                {
                    // safe to split (hostname:port or IPv4:port)
                    out.host = host_part;
                    out.port = input.substr(last_colon + 1);
                    split = true;
                }
            }

            if (!split)
            {
                out.host = input;
            }
        }

        if (out.port.empty() && !default_port.empty())
        {
            out.port = default_port;
        }

        if (!out.port.empty())
        {
            out.port_num = parse_port_number(out.port);
        }

        return out;
    }

    std::tuple<std::string, std::string> ParseEndpoint(std::string_view input, std::string_view default_port)
    {
        auto v = ParseEndpointView(input, default_port);
        return EndpointParserImpl::materialize_host_port(v);
    }

}
// namespace AqualinkAutomate::Utility
