#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>

#include "generator/jandy_message_generator_packetvalidation.h"
#include "messages/jandy_message_constants.h"
#include "utility/jandy_checksum.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Generators
{
	bool PacketValidation_ChecksumIsValid(const boost::circular_buffer<uint8_t>::iterator& start_it, const boost::circular_buffer<uint8_t>::iterator& end_it)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("JandyMessageGenerator::PacketValidation -> validate_checksum", std::source_location::current());

		const auto length = static_cast<std::size_t>(std::distance(start_it, end_it));

		// A valid Jandy packet always contains at least the DLE/STX header, a checksum byte, and the
		// DLE/ETX footer. The checksum byte sits at length-3; guard against under-length ranges so the
		// subtraction below cannot wrap around and produce an out-of-bounds iterator.
		if (3 > length)
		{
			LogTrace(Channel::Messages, [&length] { return std::format("Packet of length {} is too short to contain a checksum byte; rejecting.", length); });
			return false;
		}

		// This validation runs on the RAW, still-DLE-stuffed wire bytes (de-stuffing happens later,
		// during Deserialize()). A checksum byte whose own value is 0x10 (DLE) is stuffed on the wire
		// exactly like any other literal 0x10 in the packet, i.e. as {0x10, 0x00} -- which pushes the
		// real checksum byte one position earlier than length-3 and leaves the stuffing pad (0x00) in
		// its usual spot. Without this adjustment such a packet's checksum byte is misread as the
		// stuffing pad (0x00) and the true checksum byte gets folded into the summed region a second
		// time, always doubling the calculated value -- so a perfectly valid frame (e.g. a OneTouch
		// MessageLong display-line update) is rejected as ChecksumFailure and silently dropped. This
		// is the same class of bug already handled for a DLE-valued destination byte in
		// JandyMessageFactory::CreateFromSerialData.
		if (4 <= length)
		{
			boost::circular_buffer<uint8_t>::iterator stuffed_checksum_it = start_it + static_cast<std::ptrdiff_t>(length - 4);
			if ((Messages::HEADER_BYTE_DLE == *stuffed_checksum_it) && (0x00 == *std::next(stuffed_checksum_it)))
			{
				const uint8_t original_checksum = static_cast<uint8_t>(*stuffed_checksum_it);
				const auto calculated_checksum = Utility::JandyPacket_CalculateChecksum(start_it, stuffed_checksum_it);
				const auto checksum_is_valid = (original_checksum == calculated_checksum);

				LogTrace(Channel::Messages, [&calculated_checksum, &original_checksum, &checksum_is_valid] { return std::format("Validating packet checksum (DLE-stuffed checksum byte): calculated=0x{:02x}, original=0x{:02x} -> {}!", calculated_checksum, original_checksum, checksum_is_valid ? "Success" : "Failure"); });

				return checksum_is_valid;
			}
		}

		boost::circular_buffer<uint8_t>::iterator checksum_it = start_it + static_cast<std::ptrdiff_t>(length - 3);
		const uint8_t original_checksum = static_cast<uint8_t>(*checksum_it);
		const auto calculated_checksum = Utility::JandyPacket_CalculateChecksum(start_it, checksum_it);
		const auto checksum_is_valid = (original_checksum == calculated_checksum);

		LogTrace(Channel::Messages, [&calculated_checksum, &original_checksum, &checksum_is_valid] { return std::format("Validating packet checksum: calculated=0x{:02x}, original=0x{:02x} -> {}!", calculated_checksum, original_checksum, checksum_is_valid ? "Success" : "Failure"); });

		return checksum_is_valid;
	}

}
// namespace AqualinkAutomate::Generators
