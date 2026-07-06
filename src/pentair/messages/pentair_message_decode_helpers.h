#pragma once

#include <cstdint>
#include <span>

#include "messages/pentair_message_constants.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// Shared, bounds-checked DATA-section accessor for Pentair status decoders.
	//
	// Every concrete status message decodes a variable-length DATA section that
	// begins at Offset_DataStart and is `LEN` bytes long (LEN lives at
	// Offset_Length).  The framing/size-gate layer (PentairMessage::Deserialize)
	// has already guaranteed that the LEN-many DATA bytes are present in the span,
	// so the only per-field hazard is a status frame that advertises fewer bytes
	// than a particular field offset requires.  This helper centralises that
	// "field present?" guard so the three decoders no longer hand-roll an
	// identical capturing lambda each: a field beyond the advertised length reads
	// back as zero rather than over-running the span.
	[[nodiscard]] inline constexpr uint8_t DataByteAt(std::span<const uint8_t> message_bytes, uint8_t data_index)
	{
		if (const uint8_t data_length = message_bytes[Offset_Length]; data_index >= data_length)
		{
			return static_cast<uint8_t>(0);
		}

		return message_bytes[Offset_DataStart + data_index];
	}

	// Advertised DATA length for the frame (the LEN header byte).
	[[nodiscard]] inline constexpr uint8_t DataLengthOf(std::span<const uint8_t> message_bytes)
	{
		return message_bytes[Offset_Length];
	}

}
// namespace AqualinkAutomate::Pentair::Messages
