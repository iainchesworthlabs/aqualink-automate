#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <string>
#include <span>
#include <type_traits>
#include <vector>

#include <boost/assert.hpp>

#include "interfaces/imessage.h"
#include "interfaces/iserializable.h"
#include "devices/jandy_device_types.h"
#include "messages/jandy_message_ids.h"

namespace AqualinkAutomate::Messages
{
	template <typename Range>
	concept JandyRawMessageRange = std::ranges::random_access_range<Range> && std::same_as<std::ranges::range_value_t<Range>, uint8_t>;

	template <typename Range>
	concept MutableJandyRawMessageRange = JandyRawMessageRange<Range> && std::same_as<std::ranges::range_reference_t<Range>, uint8_t&>;

	class JandyMessage : public Interfaces::IMessage<JandyMessageIds>, public Interfaces::ISerializable
	{
	public:
		static constexpr uint8_t Index_DestinationId{ 2 };
		static constexpr uint8_t Index_MessageType{ 3 };
		static constexpr uint8_t Index_DataStart{ 4 };

	public:
		static constexpr uint8_t PACKET_HEADER_LENGTH{ 4 };
		static constexpr uint8_t PACKET_FOOTER_LENGTH{ 3 };
		static constexpr uint8_t MAXIMUM_PACKET_LENGTH{ 128 };
		static constexpr uint8_t MINIMUM_PACKET_LENGTH{ PACKET_HEADER_LENGTH + PACKET_FOOTER_LENGTH };

		explicit JandyMessage(const JandyMessageIds& msg_id);
		~JandyMessage() override = default;

		const Devices::JandyDeviceType Destination() const;
		uint8_t RawId() const;
		uint8_t MessageLength() const;
		uint8_t ChecksumValue() const;

		uint8_t MaxPermittedPacketLength() const override;
		uint8_t MinPermittedPacketLength() const override;
		std::string ToString() const override;

		// Two Deserialize overloads coexist on this type.  They are NOT competing
		// generic overloads — they form a single logical entry point split by an
		// unambiguous, disjoint parameter domain, both forwarding into the one
		// shared DeserializeFromContiguousData() implementation:
		//
		//   * Deserialize(std::span<const std::byte>&)  — the ISerializable
		//     interface contract (cold path: tests / generic serialisation).
		//   * Deserialize(const JandyRawMessageRange&)  — the hot path: a
		//     contiguous uint8_t range (a linearised circular-buffer subrange).
		//
		// A std::byte span never satisfies JandyRawMessageRange (range_value_t is
		// std::byte, not uint8_t) and a uint8_t range never converts to a
		// std::byte span, so overload resolution can never be ambiguous between
		// them.  Both names are intentional and load-bearing: the factory and the
		// device unit tests call the templated uint8_t form, while the
		// ISerializable interface (and the to-master decode test) call the
		// std::byte form.  Do not rename one without updating those call sites.
		bool Serialize(std::vector<uint8_t>& message_bytes) const final;
		virtual bool SerializeContents(std::vector<uint8_t>& message_bytes) const = 0;
		bool Deserialize(const std::span<const std::byte>& message_bytes) final;
		virtual bool DeserializeContents(std::span<const uint8_t> message_bytes) = 0;

		template <MutableJandyRawMessageRange RAW_MESSAGE_RANGE>
		[[nodiscard]] bool Serialize(RAW_MESSAGE_RANGE& raw_message) const
		{
			std::vector<uint8_t> contiguous_raw_data;
			auto result = Serialize(contiguous_raw_data);
			raw_message.reserve(contiguous_raw_data.size());
			std::ranges::copy(contiguous_raw_data, std::back_inserter(raw_message));

			return result;
		}

		/// Deserialize from a contiguous range of uint8_t (e.g. a linearized
		/// circular-buffer subrange).  After circular_buffer::linearize() the
		/// iterators address contiguous storage, so we form a span directly
		/// over the caller's memory — no heap copy required.
		///
		/// HARD PRECONDITION: the supplied range must be contiguous in memory.
		/// The JandyRawMessageRange concept only requires random_access_range
		/// (a boost::circular_buffer subrange is not statically a
		/// std::contiguous_range, so the contiguity cannot be a static_assert);
		/// the generator establishes it dynamically by calling
		/// circular_buffer::linearize() before parsing.  A debug BOOST_ASSERT
		/// verifies the storage really is contiguous so a future caller that
		/// forgets to linearize fails loudly instead of forming a span over
		/// non-contiguous memory.
		template <JandyRawMessageRange RAW_MESSAGE_RANGE>
		[[nodiscard]] bool Deserialize(const RAW_MESSAGE_RANGE& raw_message)
		{
			const auto size = std::ranges::size(raw_message);

			if (0 == size)
			{
				return DeserializeFromContiguousData(std::span<const uint8_t>{});
			}

			const auto first = std::ranges::begin(raw_message);
			const uint8_t* const base = &*first;

			// Verify the range really is laid out contiguously: the address of
			// the last element (reached by random-access advance from the first)
			// must equal base + (size - 1).  This catches a non-linearised
			// circular-buffer subrange (split across the wrap point) in debug
			// builds before a malformed span is constructed.  Advancing from
			// begin avoids relying on the range's sentinel being an iterator.
			BOOST_ASSERT_MSG(
				(base + (size - 1)) == &*std::ranges::next(first, static_cast<std::ranges::range_difference_t<RAW_MESSAGE_RANGE>>(size - 1)),
				"JandyMessage::Deserialize requires a contiguous range; the caller must linearize the circular buffer first");

			std::span<const uint8_t> raw_span(base, size);
			return DeserializeFromContiguousData(raw_span);
		}

	private:
		[[nodiscard]] bool DeserializeFromContiguousData(std::span<const uint8_t> contiguous_data);

	protected:
		bool PacketSizeIsValid(const std::span<const std::byte>& message_bytes) const;
		bool PacketFramingIsValid(std::span<const uint8_t> message_bytes) const;
		bool PacketChecksumIsValid(std::span<const uint8_t> message_bytes) const;

	protected:
		Devices::JandyDeviceType m_Destination{ 0x00 };
		uint8_t m_RawId{ 0 };
		uint8_t m_MessageLength{ 0 };
		uint8_t m_ChecksumValue{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
