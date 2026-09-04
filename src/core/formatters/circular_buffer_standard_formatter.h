#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <ostream>

#include <boost/circular_buffer.hpp>

#include "formatters/formatter_helpers.h"


namespace AqualinkAutomate
{

	std::ostream& operator<<(std::ostream& os, const boost::circular_buffer<uint8_t>& obj);

}
// namespace AqualinkAutomate

template<>
struct std::formatter<boost::circular_buffer<uint8_t>>
{
	enum class Mode
	{
		Stats,
		HexBytes
	};

	Mode m_Mode{ Mode::Stats };

	bool m_HasPrecisionLimit{ false };
	std::size_t m_PrecisionLimit{ std::numeric_limits<std::size_t>::max() };

	constexpr auto parse(std::format_parse_context& ctx)
	{
		auto it = ctx.begin();
		auto end = ctx.end();

		// "{}" or "{:}" -> stats mode
		if (it == end || *it == '}')
		{
			m_Mode = Mode::Stats;
			return it;
		}

		// If the spec starts with '.', we switch to hex-bytes mode.
		if (*it == '.')
		{
			m_Mode = Mode::HexBytes;
			++it;

			// Optional static precision digits: "{:.16}"
			std::size_t value = 0;
			bool have_digit = false;

			while (it != end && *it >= '0' && *it <= '9')
			{
				have_digit = true;
				value = (value * 10) + static_cast<std::size_t>(*it - '0');
				++it;
			}

			if (have_digit)
			{
				m_PrecisionLimit = value;
				m_HasPrecisionLimit = true;
			}
			else
			{
				// If we see "{...}" (dynamic precision like {.{}}),
				// just skip until '}' and ignore the actual value.
				if (it != end && *it == '{')
				{
					while (it != end && *it != '}')
					{
						++it;
					}
					// '}' will be consumed by the caller
				}
			}

			// We expect to be at '}' or end here.
			if (it != end && *it != '}')
			{
				throw std::format_error("Invalid format specifier for boost::circular_buffer<uint8_t>");
			}

			return it;
		}

		throw std::format_error("Invalid format specifier for boost::circular_buffer<uint8_t>");
	}

	template<typename FormatContext>
	auto format(const boost::circular_buffer<uint8_t>& buffer, FormatContext& ctx) const
	{
		auto out = ctx.out();

		switch (m_Mode)
		{
			case Mode::Stats:
			{
				const std::size_t size = buffer.size();
				const std::size_t capacity = buffer.capacity();
				const std::size_t start_index = 0;
				const std::size_t end_index = size;

				return std::format_to(out, "size={} / capacity={} (full={} empty={}) [range: start={} end={}]", size, capacity, buffer.full(), buffer.empty(), start_index, end_index);
			}

			case Mode::HexBytes:
			{
				std::size_t count = buffer.size();

				if (m_HasPrecisionLimit && m_PrecisionLimit < count)
				{
					count = m_PrecisionLimit;
				}

				// Render only the leading `count` bytes; reuse the shared hex-dump helper so the
				// per-byte loop is identical to the array/span/vector formatters.
				return AqualinkAutomate::Formatters::FormatHexBytes(buffer.begin(), buffer.begin() + static_cast<boost::circular_buffer<uint8_t>::difference_type>(count), out);
			}
		}

		return out; // unreachable
	}
};
