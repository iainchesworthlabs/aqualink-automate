#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <ranges>

#include <magic_enum/magic_enum.hpp>

#include "errors/message_errors.h"
#include "errors/protocol_errors.h"
#include "messages/pentair_message.h"
#include "messages/pentair_message_constants.h"
#include "messages/pentair_message_ids.h"
#include "types/pentair_types.h"
#include "logging/logging.h"

namespace AqualinkAutomate::Pentair::Factory
{

	// Creates a concrete PentairMessage subclass instance for a given command id,
	// falling back to PentairMessage_Unknown for any unrecognised command, then
	// deserialises a CHECKSUMMED-REGION byte range into it.
	//
	// Unlike the Jandy compile-time dispatch table, the Pentair command space is
	// small and sparse, so a simple runtime switch (CreateMessageFromCommand) is
	// used; concrete subclasses are added there as each is implemented.
	class PentairMessageFactory
	{
	public:
		PentairMessageFactory() = delete;

		[[nodiscard]] static Types::PentairMessageTypePtr CreateMessageFromCommand(Messages::PentairMessageIds id) noexcept;

		template <Messages::PentairRawMessageRange MESSAGE_BYTES_RANGE>
		[[nodiscard]] static Types::PentairExpectedMessageType CreateFromSerialData(const MESSAGE_BYTES_RANGE& message_bytes)
		{
			boost::system::error_code return_value;

			if (Messages::HEADER_LENGTH > std::ranges::size(message_bytes))
			{
				LogDebug(Logging::Channel::Messages, "Attempted to generate a Pentair message from an invalid frame: data was too short");
				return_value = make_error_code(ErrorCodes::Protocol_ErrorCodes::InvalidPacketFormat);
			}
			else
			{
				auto first_it = std::ranges::begin(message_bytes);
				const uint8_t raw_command = *(first_it + static_cast<std::ptrdiff_t>(Messages::Offset_Command));

				const Messages::PentairMessageIds command_id(
					magic_enum::enum_cast<Messages::PentairMessageIds>(raw_command)
						.value_or(Messages::PentairMessageIds::Unknown));

				LogTrace(Logging::Channel::Messages, [&command_id, &raw_command]() { return std::format("Generating: Pentair message --> {} (0x{:02x})", magic_enum::enum_name(command_id), raw_command); });

				if (Types::PentairMessageTypePtr message = CreateMessageFromCommand(command_id); !message)
				{
					LogDebug(Logging::Channel::Messages, "Generating: Failed to create Pentair message; nullptr returned");
					return_value = make_error_code(ErrorCodes::Message_ErrorCodes::Error_GeneratorFailed);
				}
				else if (!message->Deserialize(message_bytes))
				{
					LogDebug(Logging::Channel::Messages, "Failed to deserialise serial bytes into generated Pentair message");
					return_value = make_error_code(ErrorCodes::Message_ErrorCodes::Error_FailedToDeserialize);
				}
				else
				{
					LogTrace(Logging::Channel::Messages, [&message]() { return std::format("Pentair Message Contents -> {{{}}}", message->ToString()); });
					return message;
				}
			}

			return std::unexpected<boost::system::error_code>(return_value);
		}
	};

}
// namespace AqualinkAutomate::Pentair::Factory
