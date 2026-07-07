#include <array>
#include <format>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "formatters/array_standard_formatter.h"
#include "formatters/jandy_device_formatters.h"
#include "messages/jandy_message.h"
#include "messages/jandy_message_constants.h"
#include "utility/jandy_checksum.h"
#include "utility/jandy_null_handler.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{
JandyMessage::JandyMessage(const JandyMessageIds& msg_id) :
		Interfaces::IMessage<JandyMessageIds>(msg_id),
		Interfaces::ISerializable()
	{
	}


	const Devices::JandyDeviceType JandyMessage::Destination() const
	{
		return m_Destination;
	}

	uint8_t JandyMessage::RawId() const
	{
		return m_RawId;
	}

	uint8_t JandyMessage::MessageLength() const
	{
		return m_MessageLength;
	}

	uint8_t JandyMessage::ChecksumValue() const
	{
		return m_ChecksumValue;
	}

	uint8_t JandyMessage::MaxPermittedPacketLength() const
	{
		return MAXIMUM_PACKET_LENGTH;
	}

	uint8_t JandyMessage::MinPermittedPacketLength() const
	{
		return MINIMUM_PACKET_LENGTH;
	}

	std::string JandyMessage::ToString() const
	{
		return std::format(
			"Destination: {} ({}), Message Type: {} (0x{:02x})",
			magic_enum::enum_name(Destination().Class()), 
			Destination().Id(), 
			magic_enum::enum_name(IMessage::Id()),
			magic_enum::enum_integer(IMessage::Id())
		);
	}

	bool JandyMessage::Serialize(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes.emplace_back(Messages::HEADER_BYTE_DLE);
		message_bytes.emplace_back(Messages::HEADER_BYTE_STX);
		message_bytes.emplace_back(JandyMessage::m_Destination.Id()());
		message_bytes.emplace_back(magic_enum::enum_integer(IMessage::Id()));

		if (!SerializeContents(message_bytes))
		{
			return false;
		}

		const auto message_span_to_checksum = std::as_bytes(std::span<uint8_t>(message_bytes.begin(), message_bytes.size()));

		message_bytes.emplace_back(Utility::JandyPacket_CalculateChecksum(message_span_to_checksum.begin(), message_span_to_checksum.end()));
		message_bytes.emplace_back(Messages::HEADER_BYTE_DLE);
		message_bytes.emplace_back(Messages::HEADER_BYTE_ETX);
		
		Utility::JandyPacket_NullCharHandler_Serialization(message_bytes);

		LogTrace(Channel::Messages, std::format("{} Message (Raw): {}", magic_enum::enum_name(IMessage::Id()), message_bytes));

		return true;
	}

	bool JandyMessage::Deserialize(const std::span<const std::byte>& message_bytes)
	{
		if (!PacketSizeIsValid(message_bytes))
		{
			LogDebug(Channel::Messages, "Cannot deserialise JandyMessage packet; packet size is not valid");
			return false;
		}

		// Convert std::byte span to uint8_t span via a stack buffer (this
		// path is only used by the ISerializable interface, not the hot path).
		//
		// The buffer is sized by MAXIMUM_PACKET_LENGTH rather than a bare 128
		// literal so it stays coupled to the upper bound PacketSizeIsValid()
		// enforces above: that check guarantees message_bytes.size() <=
		// MAXIMUM_PACKET_LENGTH, so the transform below can never overrun the
		// buffer.  Were MAXIMUM_PACKET_LENGTH ever raised without growing this
		// buffer, the size check would admit packets the buffer could not hold.
		using ByteBuffer = std::array<uint8_t, MAXIMUM_PACKET_LENGTH>;

		static_assert(
			std::tuple_size_v<ByteBuffer> >= MAXIMUM_PACKET_LENGTH,
			"std::byte Deserialize stack buffer must be at least MAXIMUM_PACKET_LENGTH bytes to hold any packet PacketSizeIsValid() admits");

		ByteBuffer byte_buffer{};
		std::transform(message_bytes.begin(), message_bytes.end(), byte_buffer.begin(),
			[](std::byte b)
			{
				return std::to_underlying(b);
			}
		);

		return DeserializeFromContiguousData(std::span<const uint8_t>(byte_buffer.data(), message_bytes.size()));
	}

	bool JandyMessage::DeserializeFromContiguousData(std::span<const uint8_t> contiguous_data)
	{
		// Fast path: if no DLE-null escaping is present, deserialize directly
		// from the caller's memory with zero copies.
		if (contiguous_data.size() > MAXIMUM_PACKET_LENGTH)
		{
			LogDebug(Channel::Messages, "Cannot deserialise JandyMessage packet; packet exceeds maximum permitted length");
			return false;
		}

		std::array<uint8_t, MAXIMUM_PACKET_LENGTH> unescape_buffer;
		std::span<const uint8_t> packet_data = contiguous_data;

		if (Utility::JandyPacket_NeedsNullCharHandling(contiguous_data))
		{
			auto filtered_size = Utility::JandyPacket_NullCharHandler_DeserializationToSpan(contiguous_data, std::span<uint8_t>(unescape_buffer));
			packet_data = std::span<const uint8_t>(unescape_buffer.data(), filtered_size);
		}

		if (!PacketFramingIsValid(packet_data))
		{
			LogDebug(Channel::Messages, "Cannot deserialise JandyMessage packet; packet framing is not valid");
		}
		else if (!PacketChecksumIsValid(packet_data))
		{
			LogDebug(Channel::Messages, "Cannot deserialise JandyMessage packet; packet checksum is not valid");
		}
		else
		{
			m_Destination = Devices::JandyDeviceType(static_cast<uint8_t>(packet_data[Index_DestinationId]));
			m_RawId = static_cast<uint8_t>(packet_data[Index_MessageType]);
			m_MessageLength = static_cast<uint8_t>(packet_data.size());
			m_ChecksumValue = static_cast<uint8_t>(packet_data[m_MessageLength - PACKET_FOOTER_LENGTH]);

			// Update the enum ID from the wire byte so Id() returns the
			// specific enum value (e.g. IAQ_Heartbeat) rather than the
			// default-constructed value (e.g. Unknown).
			if (auto resolved = magic_enum::enum_cast<JandyMessageIds>(m_RawId); resolved.has_value())
			{
				SetId(resolved.value());
			}

			return DeserializeContents(packet_data);
		}

		return false;
	}

	bool JandyMessage::PacketSizeIsValid(const std::span<const std::byte>& message_bytes) const
	{
		bool packet_is_valid = true;

		packet_is_valid &= (MINIMUM_PACKET_LENGTH <= message_bytes.size());
		packet_is_valid &= (MAXIMUM_PACKET_LENGTH >= message_bytes.size());

		LogTrace(Channel::Messages, std::format("Packet size validation check passed: {}", packet_is_valid));

		return packet_is_valid;
	}

	bool JandyMessage::PacketFramingIsValid(std::span<const uint8_t> message_bytes) const
	{
		if (MINIMUM_PACKET_LENGTH > message_bytes.size())
		{
			LogDebug(Channel::Messages, "Attempted to validate framing on a packet with an invalid size!");
			return false;
		}
		
		bool packet_is_valid = true;

		packet_is_valid &= (message_bytes[0] == Messages::HEADER_BYTE_DLE);
		packet_is_valid &= (message_bytes[1] == Messages::HEADER_BYTE_STX);
		packet_is_valid &= (message_bytes[message_bytes.size() - 2] == Messages::HEADER_BYTE_DLE);
		packet_is_valid &= (message_bytes[message_bytes.size() - 1] == Messages::HEADER_BYTE_ETX);

		LogTrace(Channel::Messages, std::format("Packet framing validation check passed: {}", packet_is_valid));

		return packet_is_valid;
	}

	bool JandyMessage::PacketChecksumIsValid(std::span<const uint8_t> message_bytes) const
	{
		if (MINIMUM_PACKET_LENGTH > message_bytes.size())
		{
			LogDebug(Channel::Messages, "Attempted to validate checksum on a packet with an invalid size!");
			return false;
		}

		bool packet_is_valid = true;

		std::span<const uint8_t> message_span_to_checksum(message_bytes.data(), message_bytes.size() - PACKET_FOOTER_LENGTH);
		const auto expected_checksum = Utility::JandyPacket_CalculateChecksum(message_span_to_checksum.begin(), message_span_to_checksum.end());
		const auto received_checksum = message_bytes[message_bytes.size() - PACKET_FOOTER_LENGTH];

		packet_is_valid &= (expected_checksum == received_checksum);

		LogTrace(Channel::Messages, std::format("Packet checksum validation check passed: {}", packet_is_valid));

		return packet_is_valid;
	}

}
// namespace AqualinkAutomate::Messages::Jandy::Messages
