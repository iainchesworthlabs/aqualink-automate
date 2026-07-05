#pragma once

#include <cstdint>
#include <cstddef>
#include <concepts>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "interfaces/imessage.h"
#include "interfaces/iserializable.h"
#include "messages/pentair_message_ids.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// A Pentair raw-message range must be a CONTIGUOUS, sized range of uint8_t.
	// Deserialize() forms a std::span over the range's data pointer, so plain
	// random-access (e.g. std::deque) is not sufficient — contiguity is required
	// for the span to be well-formed, not merely assumed.
	template <typename Range>
	concept PentairRawMessageRange =
		std::ranges::contiguous_range<Range> &&
		std::ranges::sized_range<Range> &&
		std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, uint8_t>;

	// Base class for every Pentair RS-485 message.
	//
	// A PentairMessage owns the decoded frame header (FROM / DEST / CMD / LEN) and
	// delegates payload decode/encode to the concrete subclass via
	// (De)SerializeContents.  The 16-bit checksum is validated (not retained) by
	// whichever layer owns verification — the generator on the hot path, or the
	// ISerializable byte-span path itself.  Deserialize() consumes a
	// CHECKSUMMED REGION span — the bytes from the 0xA5 SOF through the trailing
	// two checksum bytes (the generator strips the 0xFF/0x00/0xFF preamble before
	// handing the frame over).
	class PentairMessage : public Interfaces::IMessage<PentairMessageIds>, public Interfaces::ISerializable
	{
	public:
		explicit PentairMessage(const PentairMessageIds msg_id);
		~PentairMessage() override = default;

		uint8_t From() const;
		uint8_t Destination() const;
		uint8_t RawCommand() const;
		uint8_t DataLength() const;

		uint8_t MaxPermittedPacketLength() const override;
		uint8_t MinPermittedPacketLength() const override;
		std::string ToString() const override;

		// ISerializable interface.  Serialize() produces a fully-framed frame
		// (preamble + checksummed region + checksum).  Deserialize() takes the
		// checksummed region (0xA5 .. CHK_LO).
		bool Serialize(std::vector<uint8_t>& message_bytes) const final;
		virtual bool SerializeContents(std::vector<uint8_t>& message_bytes) const = 0;
		bool Deserialize(const std::span<const std::byte>& message_bytes) final;
		virtual bool DeserializeContents(std::span<const uint8_t> message_bytes) = 0;

		// Hot-path deserialise directly from a contiguous uint8_t range (the
		// generator hands a linearised circular-buffer subrange).  The frame has
		// ALREADY been checksum-validated by the generator before reaching here, so
		// the redundant per-message checksum recompute is skipped on this path.
		template <PentairRawMessageRange RAW_MESSAGE_RANGE>
		[[nodiscard]] bool Deserialize(const RAW_MESSAGE_RANGE& raw_message)
		{
			// std::span(data, size) is well-formed because PentairRawMessageRange
			// requires a contiguous, sized range (no &*begin() contiguity gamble).
			const std::span<const uint8_t> raw_span(std::ranges::data(raw_message), std::ranges::size(raw_message));
			return DeserializeFromContiguousData(raw_span, /* verify_checksum */ false);
		}

	private:
		[[nodiscard]] bool DeserializeFromContiguousData(std::span<const uint8_t> contiguous_data, bool verify_checksum);

	protected:
		bool FrameSizeIsValid(std::span<const uint8_t> message_bytes) const;
		bool FrameChecksumIsValid(std::span<const uint8_t> message_bytes) const;

	protected:
		uint8_t m_From{0};
		uint8_t m_Destination{0};
		uint8_t m_RawCommand{0};
		uint8_t m_DataLength{0};
	};

}
// namespace AqualinkAutomate::Pentair::Messages
