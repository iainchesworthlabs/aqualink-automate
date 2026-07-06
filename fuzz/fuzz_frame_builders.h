#pragma once

//
// Shared frame builders for the protocol fuzz harnesses (and the frame-builder
// round-trip regression test).
//
// A fuzzer that fed purely random bytes into the message factories would almost
// never get past framing + checksum validation, so the interesting payload-decode
// paths (each message type's DeserializeContents) would stay uncovered.  These
// helpers wrap an arbitrary (address bytes + payload) triple in a fully-framed,
// checksum-correct, wire-escaped packet — mirroring the real serialisers — so a
// mutated payload reaches the deserialiser under test.  The wire encoders are
// reused verbatim (JandyPacket_NullCharHandler_Serialization, the Jandy/Pentair
// checksum helpers) so a built frame round-trips through the real Deserialize path.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "messages/jandy_message_constants.h"
#include "utility/jandy_checksum.h"
#include "utility/jandy_null_handler.h"

#include "messages/pentair_message_constants.h"
#include "utility/pentair_checksum.h"

namespace AqualinkAutomate::Fuzzing
{

	//
	// Build a fully-framed, checksum-valid, DLE-null-escaped Jandy packet around an
	// arbitrary (destination, message-type, payload).  This mirrors
	// JandyMessage::Serialize exactly — the checksum is computed over the unescaped
	// header+payload and the DLE escaping is applied afterwards — so the frame
	// round-trips through JandyMessage::Deserialize and reaches DeserializeContents.
	//
	[[nodiscard]] inline std::vector<uint8_t> BuildJandyFrame(uint8_t destination, uint8_t message_type, std::span<const uint8_t> payload)
	{
		namespace M = AqualinkAutomate::Messages;

		std::vector<uint8_t> frame;
		frame.reserve(payload.size() + 8U);

		frame.push_back(M::HEADER_BYTE_DLE);
		frame.push_back(M::HEADER_BYTE_STX);
		frame.push_back(destination);
		frame.push_back(message_type);
		frame.insert(frame.end(), payload.begin(), payload.end());

		// Checksum covers everything emitted so far (DLE STX dest type payload),
		// identical to JandyMessage::Serialize.
		const uint8_t checksum = AqualinkAutomate::Utility::JandyPacket_CalculateChecksum(frame.begin(), frame.end());
		frame.push_back(checksum);
		frame.push_back(M::HEADER_BYTE_DLE);
		frame.push_back(M::HEADER_BYTE_ETX);

		// Escape literal DLE bytes in the payload region exactly as the wire encoder
		// does; the deserialiser reverses this before validating the checksum.
		AqualinkAutomate::Utility::JandyPacket_NullCharHandler_Serialization(frame);

		return frame;
	}

	//
	// Build a fully-framed, checksum-valid Pentair packet (0xFF 0x00 0xFF 0xA5 ...)
	// around an arbitrary (from, dest, command, data).  LEN is set to the actual
	// data length so PentairMessage::FrameSizeIsValid accepts it; data is clamped to
	// the single-byte LEN maximum of 255.  Mirrors PentairMessage::Serialize.
	//
	[[nodiscard]] inline std::vector<uint8_t> BuildPentairFrame(uint8_t from, uint8_t dest, uint8_t command, std::span<const uint8_t> data)
	{
		namespace P = AqualinkAutomate::Pentair::Messages;

		std::vector<uint8_t> frame;
		frame.reserve(data.size() + 9U);

		// Preamble (NOT part of the checksummed region).
		frame.push_back(P::PREAMBLE_BYTE_FF);
		frame.push_back(P::PREAMBLE_BYTE_00);
		frame.push_back(P::PREAMBLE_BYTE_FF);

		// The 0xA5 SOF opens the checksummed region.
		const std::size_t checksum_region_start = frame.size();
		frame.push_back(P::START_OF_FRAME);
		frame.push_back(from);
		frame.push_back(dest);
		frame.push_back(command);

		const uint8_t len = static_cast<uint8_t>(std::min<std::size_t>(data.size(), 255U));
		frame.push_back(len);
		frame.insert(frame.end(), data.begin(), data.begin() + len);

		const std::span<const uint8_t> checksum_region(frame.data() + checksum_region_start, frame.size() - checksum_region_start);
		const uint16_t checksum = AqualinkAutomate::Pentair::Utility::PentairPacket_CalculateChecksum_FromRange(checksum_region);
		AqualinkAutomate::Pentair::Utility::AppendBigEndianChecksum(frame, checksum);

		return frame;
	}

	//
	// The Pentair factory (CreateFromSerialData) operates on the CHECKSUMMED REGION
	// only — the frame from the 0xA5 SOF onward, without the 3-byte preamble.  Given
	// a full frame from BuildPentairFrame, return a span over that region.
	//
	[[nodiscard]] inline std::span<const uint8_t> PentairChecksummedRegion(const std::vector<uint8_t>& full_frame)
	{
		constexpr std::size_t PREAMBLE_LEAD = 3U; // 0xFF 0x00 0xFF before the 0xA5 SOF
		if (full_frame.size() <= PREAMBLE_LEAD)
		{
			return {};
		}
		return std::span<const uint8_t>(full_frame.data() + PREAMBLE_LEAD, full_frame.size() - PREAMBLE_LEAD);
	}

}
// namespace AqualinkAutomate::Fuzzing
