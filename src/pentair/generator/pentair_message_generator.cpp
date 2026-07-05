#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
#include <vector>

#include "errors/message_errors.h"
#include "errors/protocol_errors.h"
#include "factories/pentair_message_factory.h"
#include "generator/pentair_message_generator.h"
#include "messages/pentair_message_constants.h"
#include "utility/pentair_checksum.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Generators
{

	namespace
	{
		// The 0xFF 0x00 0xFF 0xA5 sequence that opens every Pentair frame.
		constexpr std::array<uint8_t, 4> PREAMBLE = Messages::FRAME_PREAMBLE;
	}

	//=========================================================================
	// CONSUME-OR-DEFER CONTRACT (see MessageGeneratorRegistry::GenerateMessage)
	//
	// This generator returns one of three classes of result to the shared
	// registry, which runs co-registered generators in priority order:
	//
	//   * value                       -> a frame was decoded and consumed.
	//   * WaitingForMoreData          -> "NOT my protocol": no Pentair preamble
	//                                     is present, the buffer is UNTOUCHED, so
	//                                     the registry must TRY THE NEXT generator
	//                                     (e.g. Jandy) on the same bytes.
	//   * DataAvailableToProcess      -> "MY frame, INCOMPLETE": a preamble has
	//                                     been positively located but the frame is
	//                                     not fully buffered yet.  The buffer is
	//                                     UNTOUCHED.  The registry must NOT try the
	//                                     next generator (that generator — Jandy —
	//                                     clears the whole buffer when it finds no
	//                                     DLE/STX, which would destroy this in-
	//                                     flight frame that merely spans multiple
	//                                     read chunks).  The protocol task treats a
	//                                     non-buffer-shrinking DataAvailableToProcess
	//                                     as a quiet "await more data" break.
	//   * ChecksumFailure             -> a complete frame failed validation; the
	//                                     bytes up to and including the SOF are
	//                                     consumed so any leading (possibly Jandy)
	//                                     bytes are re-offered on the next pass.
	//
	// The discriminator between "not mine" and "mine but incomplete" is whether a
	// preamble was located, NOT the buffer-mutation state (both leave the buffer
	// untouched) — so the two cases MUST use distinct error codes.
	//=========================================================================
	Types::PentairExpectedMessageType GenerateMessageFromRawData(boost::circular_buffer<uint8_t>& serial_data)
	{
		using ErrorCodes::Protocol_ErrorCodes;

		if (serial_data.empty())
		{
			return std::unexpected(make_error_code(Protocol_ErrorCodes::WaitingForMoreData));
		}

		// Locate the Pentair preamble.  If it is not present, leave the buffer
		// untouched and DEFER TO OTHER PROTOCOLS (WaitingForMoreData = try-next).
		auto preamble_it = std::ranges::search(serial_data, PREAMBLE).begin();
		if (serial_data.end() == preamble_it)
		{
			LogTrace(Channel::Messages, "No Pentair preamble found in serial buffer; deferring to other protocols");
			return std::unexpected(make_error_code(Protocol_ErrorCodes::WaitingForMoreData));
		}

		// Index of the first preamble byte (0xFF) and of the 0xA5 SOF that is its
		// last byte; the checksummed region begins at the SOF.  Use indices (not
		// stored iterators) for all subsequent buffer access/erase so a later
		// linearize() — should the buffer ever be non-linearized — cannot leave us
		// holding a stale iterator.
		const auto preamble_index = static_cast<std::size_t>(std::distance(serial_data.begin(), preamble_it));
		const std::size_t sof_index = preamble_index + (PREAMBLE.size() - 1);
		const auto sof_it = preamble_it + (PREAMBLE.size() - 1);
		const auto bytes_from_sof = static_cast<std::size_t>(std::distance(sof_it, serial_data.end()));

		// Need at least the fixed header to read the LEN field.  A preamble HAS
		// been located, so the bytes belong to a Pentair frame in flight — return
		// DataAvailableToProcess (NOT WaitingForMoreData) so the registry stops
		// here and does not let a destructive co-generator clear our partial frame.
		if (Messages::HEADER_LENGTH > bytes_from_sof)
		{
			LogTrace(Channel::Messages, "Pentair preamble found but header incomplete; awaiting more serial data");
			return std::unexpected(make_error_code(Protocol_ErrorCodes::DataAvailableToProcess));
		}

		const uint8_t data_length = *(sof_it + Messages::Offset_Length);
		const std::size_t frame_region_size =
			static_cast<std::size_t>(Messages::HEADER_LENGTH) + data_length + Messages::CHECKSUM_LENGTH;

		// Whole checksummed region (0xA5 .. CHK_LO) must be present.  Same as
		// above: the frame is identified but still arriving across reads, so DEFER
		// without discarding rather than falling through to a try-next clear().
		if (frame_region_size > bytes_from_sof)
		{
			LogTrace(Channel::Messages, [&frame_region_size, &bytes_from_sof] { return std::format("Pentair frame incomplete: need {} bytes from SOF, have {}; awaiting more data", frame_region_size, bytes_from_sof); });
			return std::unexpected(make_error_code(Protocol_ErrorCodes::DataAvailableToProcess));
		}

		// The protocol task linearizes the circular buffer before parsing, so the
		// frame region is almost always contiguous and can be referenced in-place
		// via a span (zero copy).  Fall back to a copy only if the buffer happens
		// to be non-linearized (the region would wrap the ring).
		std::span<const uint8_t> region_view;
		std::vector<uint8_t> frame_copy;

		if (serial_data.is_linearized())
		{
			region_view = std::span<const uint8_t>(serial_data.linearize() + sof_index, frame_region_size);
		}
		else
		{
			frame_copy.reserve(frame_region_size);
			std::copy_n(sof_it, frame_region_size, std::back_inserter(frame_copy));
			region_view = std::span<const uint8_t>(frame_copy.data(), frame_copy.size());
		}

		// Validate the checksum before constructing a message.
		const std::span<const uint8_t> region_to_checksum = region_view.first(region_view.size() - Messages::CHECKSUM_LENGTH);
		const uint16_t expected_checksum = Utility::PentairPacket_CalculateChecksum_FromRange(region_to_checksum);

		if (const auto received_checksum = static_cast<uint16_t>(
				(static_cast<uint16_t>(region_view[region_view.size() - Messages::CHECKSUM_LENGTH]) << 8) |
				static_cast<uint16_t>(region_view[region_view.size() - 1]));
			expected_checksum != received_checksum)
		{
			LogDebug(Channel::Messages, [&expected_checksum, &received_checksum] { return std::format("Pentair frame failed checksum check (expected 0x{:04x}, received 0x{:04x}); discarding frame", expected_checksum, received_checksum); });

			// Erase ONLY the 4-byte preamble (0xFF 0x00 0xFF 0xA5), leaving both the
			// leading region AHEAD of it and the frame's DATA/checksum bytes behind
			// it in place.  This RE-OFFERS any leading bytes — which may contain a
			// complete frame of another protocol (Jandy) — instead of discarding
			// them.  Dropping the preamble (incl. the SOF) guarantees forward
			// progress: the same preamble cannot be re-matched on the next pass.
			const auto preamble_first = serial_data.begin() + static_cast<std::ptrdiff_t>(preamble_index);
			serial_data.erase(preamble_first, preamble_first + static_cast<std::ptrdiff_t>(PREAMBLE.size()));
			return std::unexpected(make_error_code(Protocol_ErrorCodes::ChecksumFailure));
		}

		// Build and deserialise the message from the validated region.  The factory
		// fully decodes into the message's own storage before returning, so the
		// region_view (which may alias serial_data) is safe to erase afterwards.
		auto message = Factory::PentairMessageFactory::CreateFromSerialData(region_view);

		// Consume the preamble + validated frame region, but PRESERVE any bytes
		// ahead of the preamble.  On a mixed bus those leading bytes can be a
		// complete frame of another protocol (Jandy) that simply arrived ahead of
		// this Pentair frame in the same read; re-offering them lets the registry
		// hand them to the co-generator on the next parse instead of silently
		// dropping them.
		const auto preamble_first = serial_data.begin() + static_cast<std::ptrdiff_t>(preamble_index);
		serial_data.erase(preamble_first, preamble_first + static_cast<std::ptrdiff_t>(PREAMBLE.size() - 1 + frame_region_size));

		return message;
	}

}
// namespace AqualinkAutomate::Pentair::Generators
