#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>

#include "errors/protocol_errors.h"
#include "factories/jandy_message_factory.h"
#include "factories/jandy_message_factory_registration.h"
#include "generator/jandy_message_generator.h"
#include "generator/jandy_message_generator_buffercleanup.h"
#include "generator/jandy_message_generator_packetprocessing.h"
#include "generator/jandy_message_generator_packetvalidation.h"
#include "types/jandy_types.h"
#include "utility/jandy_checksum.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Generators
{

	Types::JandyExpectedMessageType GenerateMessageFromRawData(boost::circular_buffer<uint8_t>& serial_data)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("JandyMessageGenerator::GenerateMessage", std::source_location::current());
		zone->Value(serial_data.size());

		if (serial_data.empty())
		{
			LogTrace(Channel::Messages, "The internal serial data buffer is empty; ignoring message generation.");
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData));
		}

		// Single scan: find all packet boundaries at once.
		auto locations = PacketProcessing_FindAllPacketLocations(serial_data);

		if (!locations.HasPacketStart)
		{
			// Clear the internal buffer....it doesn't contain anything useful.
			serial_data.clear();

			LogTrace(Channel::Messages, "The internal serial data buffer does not contain a packet start sequence; ignoring message generation");
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData));
		}

		// Check if end-of-packet is within max distance; may erase data and invalidate iterators.
		if (const bool buffer_was_mutated = BufferCleanUp_HasEndOfPacketWithinMaxDistance(serial_data, locations); buffer_was_mutated)
		{
			// Cleanup erased bytes and invalidated the cached iterators; a fresh scan is required.
			locations = PacketProcessing_FindAllPacketLocations(serial_data);
		}

		PacketProcessing_OutputSerialDataToConsole(serial_data, locations);

		if (!locations.HasPacketStart)
		{
			// If no header found, clear all stored serial bytes and terminate
			serial_data.clear();

			LogTrace(Channel::Messages, "Cannot find start of packet in serial data; clearing serial buffer.");
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData));
		}

		if ((locations.HasPacketEnd) && (locations.HasSecondPacketStart) && (0 > std::distance(locations.p1_end + 2, locations.p2_start))) // Account for the DLE,ETX bytes
		{
			auto packet_one_end_index = std::distance(serial_data.begin(), locations.p1_end);
			auto packet_two_start_index = std::distance(serial_data.begin(), locations.p2_start);

			// There's an DLE,STX prior to this packets ETX,DLE...this is an error state.

			LogTrace(Channel::Messages, [&packet_one_end_index, &packet_two_start_index] { return std::format("Searching for overlapping packets: packet one end: {}, packet two start: {}", packet_one_end_index, packet_two_start_index); });
			LogTrace(Channel::Messages, "Found the start of a second packet before the current packet terminator bytes.");

			// Clear all stored serial bytes up to the start of the new packet.
			serial_data.erase(serial_data.begin(), serial_data.begin() + packet_two_start_index);
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::OverlappingPackets));
		}
		else if ((!locations.HasPacketEnd) && (locations.HasSecondPacketStart))
		{
			auto packet_two_start_index = std::distance(serial_data.begin(), locations.p2_start);

			LogTrace(Channel::Messages, "Found the start of a second packet before the current packet terminator bytes.");

			// Clear all stored serial bytes up to the start of the new packet.
			serial_data.erase(serial_data.begin(), serial_data.begin() + packet_two_start_index);
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::OverlappingPackets));
		}
		else if (!locations.HasPacketEnd)
		{
			// The packet is not complete....do nothing at this point.
			LogTrace(Channel::Messages, "End of current packet not yet received; awaiting more serial data.");
			return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData));
		}
		else
		{
			// Don't care, we may have found an DLE,STX but it is _after_ this packet's ETX,DLE

			// Step 3 -> Validate that the packet is correctly formed and has not been corrupted
			if (!PacketValidation_ChecksumIsValid(locations.p1_start, locations.p1_end + 2))
			{
				// Step 3a -> If checksum fails, clear bytes and terminate (which happens below)...
				LogDebug(Channel::Messages, "Packet failed checksum check; removing packet data from serial buffer.");
				BufferCleanUp_ClearBytesFromBeginToPos(serial_data, locations.p1_end + 2);  // Account for the DLE,ETX bytes

				return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::ChecksumFailure));
			}
			else
			{
				// Step 3b -> If checksum passes, convert to message, clear all bytes, go back to Step 2
				auto message_range = std::ranges::subrange(locations.p1_start, locations.p1_end + 2);
				auto message = Factory::JandyMessageFactoryT::CreateFromSerialData(message_range);

				BufferCleanUp_ClearBytesFromBeginToPos(serial_data, locations.p1_end + 2);  // Account for the DLE,ETX bytes
				return message;
			}
		}

		// Getting here means there's been a problem processing the data...give up, erase the buffer, and ask for more data.
		serial_data.clear();

		LogDebug(Channel::Messages, "Unexpected failure while processing the internal serial data buffer; clearing serial data.");
		return std::unexpected<boost::system::error_code>(make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData));
	}

}
// namespace AqualinkAutomate::Generators
