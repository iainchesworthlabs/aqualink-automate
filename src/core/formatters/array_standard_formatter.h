#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <ostream>
#include <span>
#include <vector>

#include "formatters/formatter_helpers.h"

namespace AqualinkAutomate::Formatters
{

	// Single shared hex-dump formatter for any byte range. The per-byte loop lives in
	// AqualinkAutomate::Formatters::FormatHexBytes so it is identical to the circular-buffer
	// formatter and the old magic-16 array loop is gone.
	//
	// NOTE: in C++23 std::format provides a *range* formatter for std::array / std::span /
	// std::vector, so a concept-constrained partial std::formatter<R, char> specialisation would be
	// ambiguous against it. Explicit full specialisations (below) are more specialised than the
	// library's range partial specialisation and therefore win partial ordering unambiguously, so
	// each formatted byte type derives from this one implementation instead.
	template<ByteRange R>
	struct ByteRangeHexFormatter
	{
		constexpr auto parse(std::format_parse_context& ctx)
		{
			return ctx.begin();
		}

		template<typename Context>
		auto format(const R& bytes, Context& ctx) const
		{
			return AqualinkAutomate::Formatters::FormatHexBytes(bytes, ctx.out());
		}
	};

}
// AqualinkAutomate::Formatters

namespace AqualinkAutomate
{

	std::ostream& operator<<(std::ostream& os, const std::array<uint8_t, 16>& obj);
	std::ostream& operator<<(std::ostream& os, const std::span<uint8_t>& obj);
	std::ostream& operator<<(std::ostream& os, const std::vector<uint8_t>& obj);

}
// namespace AqualinkAutomate

template<>
struct std::formatter<std::array<uint8_t, 16>>
	: AqualinkAutomate::Formatters::ByteRangeHexFormatter<std::array<uint8_t, 16>>
{
};

template<>
struct std::formatter<std::span<uint8_t>>
	: AqualinkAutomate::Formatters::ByteRangeHexFormatter<std::span<uint8_t>>
{
};

template<>
struct std::formatter<std::span<const uint8_t>>
	: AqualinkAutomate::Formatters::ByteRangeHexFormatter<std::span<const uint8_t>>
{
};

template<>
struct std::formatter<std::vector<uint8_t>>
	: AqualinkAutomate::Formatters::ByteRangeHexFormatter<std::vector<uint8_t>>
{
};
