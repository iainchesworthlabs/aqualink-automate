#include "messages/jandy_message.h"
#include "messages/jandy_message_constants.h"
#include "utility/jandy_null_handler.h"

namespace AqualinkAutomate::Utility
{

	void JandyPacket_NullCharHandler_Serialization(std::vector<uint8_t>& message_bytes)
	{
		if (Messages::JandyMessage::MINIMUM_PACKET_LENGTH > message_bytes.size())
		{
			// Packet is too small (technically an error but don't handle it here).
		}
		else
		{
			auto start_it = std::next(message_bytes.begin(), 2);

			// The escape scan covers destination, type, payload, AND the checksum byte --
			// everything except the trailing DLE/ETX footer, which must stay a literal,
			// un-stuffed frame marker. The checksum byte is included because real Jandy
			// hardware DLE-stuffs it exactly like any other content byte whenever its
			// computed value happens to be 0x10; leaving it out here previously meant this
			// app's own outgoing packets sent an un-escaped checksum byte whenever the sum
			// happened to be 0x10, which a real receiver (which does escape it) would
			// misframe on the wire.
			auto end_it = std::next(message_bytes.rbegin(), 2).base();

			for (auto it = start_it; it != end_it; ++it)
			{
				if (Messages::HEADER_BYTE_DLE  == *it)
				{
					it = message_bytes.insert(std::next(it), 0x00);
					end_it = std::next(message_bytes.rbegin(), 2).base();
				}
			}
		}
	}

	void JandyPacket_NullCharHandler_Deserialization(std::vector<uint8_t>& message_bytes)
	{
		bool last_byte_was_0x10 = false;

		std::erase_if
		(
			message_bytes,
			[&last_byte_was_0x10](uint8_t current_byte) -> bool
			{
				bool should_remove = (last_byte_was_0x10 && (0x00 == current_byte));
				last_byte_was_0x10 = (0x10 == current_byte);
				return should_remove;
			}
		);
	}

	bool JandyPacket_NeedsNullCharHandling(std::span<const uint8_t> message_bytes)
	{
		for (std::size_t i = 0; i + 1 < message_bytes.size(); ++i)
		{
			if (message_bytes[i] == 0x10 && message_bytes[i + 1] == 0x00)
			{
				return true;
			}
		}
		return false;
	}

	std::size_t JandyPacket_NullCharHandler_DeserializationToSpan(std::span<const uint8_t> input, std::span<uint8_t> output)
	{
		std::size_t out_pos = 0;
		bool last_byte_was_0x10 = false;

		for (auto byte : input)
		{
			bool should_skip = (last_byte_was_0x10 && (0x00 == byte));
			last_byte_was_0x10 = (0x10 == byte);

			if (!should_skip)
			{
				// Bound the write so a caller-supplied output span that is
				// smaller than the (de-stuffed) input can never overflow.  On
				// the production path the output buffer is sized to
				// MAXIMUM_PACKET_LENGTH and de-stuffing only ever removes
				// bytes, so this clamp is purely defensive; if it ever trips
				// the returned size is clamped to what actually fits.
				if (out_pos >= output.size())
				{
					break;
				}

				output[out_pos++] = byte;
			}
		}

		return out_pos;
	}

}
// namespace AqualinkAutomate::Utility
