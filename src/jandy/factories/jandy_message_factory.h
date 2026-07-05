#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "concepts/jandy_message_type.h"
#include "errors/message_errors.h"
#include "errors/protocol_errors.h"
#include "formatters/jandy_device_formatters.h"
#include "formatters/jandy_message_formatters.h"
#include "messages/jandy_message_constants.h"
#include "messages/jandy_message_ids.h"
#include "types/jandy_types.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Factory
{

	template<Concepts::JandyMessageType MESSAGE_TYPE>
	struct MessageCreator
	{
		[[nodiscard]] static constexpr Types::JandyMessageTypePtr Create() noexcept
		{
			LogTrace(Channel::Messages, []{ return std::format("Creating message instance for message"); });
			return std::make_shared<MESSAGE_TYPE>();
		}
	};

	// NOTE: the third template parameter is retained for source compatibility with
	// the REGISTER_HOT_MESSAGE / REGISTER_MESSAGE macros declared in
	// jandy_message_factory_registration.h.  Message resolution is now uniformly
	// O(1) via the cold table, so the hot/cold distinction is no longer used here.
	template<Concepts::JandyMessageType MESSAGE_TYPE, Messages::JandyMessageIds ID, bool /*Unused_HotPath*/ = false>
	struct MessageRegistration
	{
		using MessageType = MESSAGE_TYPE;

		static constexpr Messages::JandyMessageIds id = ID;

		[[nodiscard]] constexpr Types::JandyMessageTypePtr Create() const noexcept
		{
			LogTrace(Channel::Messages, []{ return std::format("Retrieving message creator for message id: {}", id); });
			return MessageCreator<MESSAGE_TYPE>::Create();
		}
	};

	struct HashTableEntry
	{
		std::optional<Messages::JandyMessageIds> id;
		Types::JandyMessageTypePtr(*creator)() noexcept;

		constexpr HashTableEntry() noexcept :
			id(std::nullopt),
			creator(nullptr)
		{
		}

		template<Concepts::JandyMessageType T>
		constexpr HashTableEntry(Messages::JandyMessageIds msg_id) noexcept :
			id(msg_id),
			creator(&MessageCreator<T>::Create)
		{
		}
	};

	template<typename... Registrations>
	consteval auto BuildMessageTable() noexcept
	{
		std::array<HashTableEntry, 256> cold_table{};

		auto insert = [&](auto tag)
			{
				using R = typename decltype(tag)::type;
				constexpr auto message_id = R::id;

				auto& ce = cold_table[static_cast<std::size_t>(magic_enum::enum_integer(message_id))];
				ce.id = message_id;
				ce.creator = &MessageCreator<typename R::MessageType>::Create;
			};

		(insert(std::type_identity<Registrations>{}), ...);

		return cold_table;
	}

	template<typename... Registrations>
	class JandyMessageFactory {
	private:
		static constexpr auto cold_table = BuildMessageTable<Registrations...>();
		static constexpr std::size_t registry_size = sizeof...(Registrations);

	public:
		JandyMessageFactory() = delete;

		[[nodiscard]] static Types::JandyMessageTypePtr CreateMessageFromRaw(Messages::JandyMessageIds id) noexcept
		{
			LogDebug(Channel::Messages, [id]{ return std::format("Creating message type from message id; id -> {}", id); });

			const auto& entry = cold_table[static_cast<std::size_t>(magic_enum::enum_integer(id))];
			if (entry.id.has_value())
			{
				LogTrace(Channel::Messages, [id]{ return std::format("Message type (id: {}) creator located in message table", id); });
				return entry.creator();
			}

			LogWarning(Channel::Messages, [id]{ return std::format("Could not find suitable message type creator for type {}", id); });
			return nullptr;
		}

		// Runtime creation from serial data
		template <std::ranges::random_access_range MESSAGE_BYTES_RANGE>
			requires std::same_as<std::ranges::range_value_t<MESSAGE_BYTES_RANGE>, uint8_t>
		[[nodiscard]] static Types::JandyExpectedMessageType CreateFromSerialData(const MESSAGE_BYTES_RANGE& message_bytes)
		{
			boost::system::error_code return_value;

			// Require at least a full minimum packet so the message-type byte at
			// Index_MessageType (and the trailing framing) can be read safely.
			if (Messages::JandyMessage::MINIMUM_PACKET_LENGTH > std::ranges::size(message_bytes))
			{
				LogDebug(Channel::Messages, "Attempted to generate a message from an invalid packet: data was too short");
				return_value = make_error_code(ErrorCodes::Protocol_ErrorCodes::InvalidPacketFormat);
			}
			else
			{
				auto first_it = std::ranges::begin(message_bytes);

				// Pick the message-type byte for type selection. NOTE: this reads the raw,
				// still-DLE-stuffed bytes (the message's own Deserialize() de-stuffs before
				// reading its fields). A destination byte equal to the DLE framing byte (0x10)
				// is stuffed on the wire as {0x10, 0x00}, which pushes the message-type byte one
				// position later than Index_MessageType. Without this adjustment every frame to a
				// device at address 0x10 (e.g. a spa-side Dual Spa Switch) would be typed by the
				// inserted 0x00 -- i.e. mis-typed as a Probe -- so the device is never recognised
				// and its commands never decoded.
				auto type_offset = static_cast<std::ptrdiff_t>(Messages::JandyMessage::Index_MessageType);
				if (const uint8_t dest_byte = *(first_it + static_cast<std::ptrdiff_t>(Messages::JandyMessage::Index_DestinationId)); (Messages::HEADER_BYTE_DLE == dest_byte) && (0x00 == *(first_it + type_offset)))
				{
					++type_offset;
				}
				auto type_it = first_it + type_offset;
				const uint8_t raw_message_type = *type_it;

				const Messages::JandyMessageIds message_type(magic_enum::enum_cast<Messages::JandyMessageIds>(raw_message_type)
					.value_or(Messages::JandyMessageIds::Unknown));

				LogTrace(Channel::Messages, [message_type]{ return std::format("Generating: Jandy message --> {} (0x{:02x})", magic_enum::enum_name(message_type), std::to_underlying(message_type)); });

				if (Types::JandyMessageTypePtr message = CreateMessageFromRaw(message_type); !message)
				{
					LogDebug(Channel::Messages, "Generating: Failed to create Jandy message; nullptr returned");
					return_value = make_error_code(ErrorCodes::Message_ErrorCodes::Error_GeneratorFailed);
				}
				else if (!message->Deserialize(message_bytes))
				{
					LogDebug(Channel::Messages, "Failed to deserialise serial bytes into generated message");
					return_value = make_error_code(ErrorCodes::Message_ErrorCodes::Error_FailedToDeserialize);
				}
				else
				{
					LogTrace(Channel::Messages, [&message]{ return std::format("Message Contents -> {{{}}}", *message); });
					return message;
				}
			}

			return std::unexpected<boost::system::error_code>(return_value);
		}

		// Query functions
		[[nodiscard]] static consteval std::size_t RegisteredCount() noexcept
		{
			return registry_size;
		}
	};

}
// namespace AqualinkAutomate::Factory
