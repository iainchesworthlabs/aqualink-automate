#pragma once

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <ranges>
#include <span>
#include <vector>

namespace AqualinkAutomate::Pentair::Utility
{

	// Pentair frames are a pure uint8_t wire protocol, so the checksum helper is
	// deliberately narrowed to byte ranges (no std::byte / wider-integral
	// genericity is needed) and is usable in constant expressions.
	template <typename Range>
	concept ByteRange =
		std::ranges::input_range<Range> &&
		std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, uint8_t>;

	// Compute the Pentair 16-bit frame checksum: the running sum of every byte in
	// the range (the checksummed region runs from the 0xA5 SOF through the final
	// DATA byte).  The wire encoding is big-endian (high byte first, low byte
	// second), so callers append (checksum >> 8) then (checksum & 0xFF).
	template <ByteRange Range>
	[[nodiscard]] constexpr uint16_t PentairPacket_CalculateChecksum_FromRange(Range&& message_bytes)
	{
		uint32_t checksum = 0;

		for (const uint8_t value : message_bytes)
		{
			checksum += static_cast<uint32_t>(value);
		}

		return static_cast<uint16_t>(checksum & 0xFFFF);
	}

	// Read the trailing big-endian 16-bit checksum (CHK_HI then CHK_LO) that
	// terminates a Pentair frame.  `message_bytes` is the full checksummed region
	// PLUS the two checksum bytes; the caller must guarantee at least two bytes.
	[[nodiscard]] constexpr uint16_t ReadBigEndianChecksum(std::span<const uint8_t> message_bytes)
	{
		const std::size_t hi_index = message_bytes.size() - 2;
		return static_cast<uint16_t>(
			(static_cast<uint16_t>(message_bytes[hi_index]) << 8) |
			static_cast<uint16_t>(message_bytes[hi_index + 1]));
	}

	// Append a 16-bit checksum to a frame buffer in big-endian wire order (high
	// byte first, then low byte).
	constexpr void AppendBigEndianChecksum(std::vector<uint8_t>& message_bytes, uint16_t checksum)
	{
		message_bytes.emplace_back(static_cast<uint8_t>((checksum >> 8) & 0xFF));
		message_bytes.emplace_back(static_cast<uint8_t>(checksum & 0xFF));
	}

}
// namespace AqualinkAutomate::Pentair::Utility
