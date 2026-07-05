#include <algorithm>
#include <format>

#include "generator/jandy_message_generator_buffercleanup.h"
#include "generator/jandy_message_generator_startendsequence.h"
#include "messages/jandy_message.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Generators
{

	void BufferCleanUp_ClearBytesFromBeginToPos(boost::circular_buffer<uint8_t>& serial_data, const boost::circular_buffer<uint8_t>::iterator& position)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("JandyMessageGenerator::BufferCleanUp -> clearing_bytes", std::source_location::current());

		// Erase all bytes to the end of this packet.
		auto packet_one_end_index = std::distance(serial_data.begin(), position);
		LogTrace(Channel::Messages, "Packet processing complete; removing packet data from serial buffer.");
		LogTrace(Channel::Messages, [&packet_one_end_index] { return std::format("Clearing {} elements from the serial data.", packet_one_end_index); });
		serial_data.erase(serial_data.begin(), serial_data.begin() + packet_one_end_index);
	}

	bool BufferCleanUp_HasEndOfPacketWithinMaxDistance(boost::circular_buffer<uint8_t>& serial_data, const PacketLocations& locations)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("JandyMessageGenerator::BufferCleanUp -> has_end_of_packet", std::source_location::current());

		bool buffer_was_mutated = false;

		if (Messages::JandyMessage::MAXIMUM_PACKET_LENGTH >= serial_data.size() || !locations.HasPacketStart)
		{
			// Not enough data in the buffer or there doesn't appear to be a packet in this data...ignore.
		}
		else
		{
			// Do a bunch of sense checks...
			const auto distance_between_start_and_end = std::distance(locations.p1_start, locations.p1_end);
			if (0 == distance_between_start_and_end)
			{
				LogDebug(Channel::Messages, "Attempted to action a clean-up here the start and end location were the same...ignoring");
			}
			else if (Messages::JandyMessage::MAXIMUM_PACKET_LENGTH < (distance_between_start_and_end + PACKET_END_SEQUENCE.size()))
			{
				LogDebug(Channel::Messages, [] { return std::format("Packet end sequence not present within {} bytes of start sequence; ignoring this particular packet", Messages::JandyMessage::MAXIMUM_PACKET_LENGTH); });

				// The distance between the start and the end is larger than the maximum packet size...erase
				// everything up to the end iterator. Only advance past the end-of-packet sequence bytes when an
				// actual packet-end was located; otherwise p1_end == serial_data.end() and advancing would push
				// the erase endpoint past end() (out-of-bounds).
				auto serial_data_begin = serial_data.begin();
				auto erase_end = locations.HasPacketEnd ? (locations.p1_end + PACKET_END_SEQUENCE.size()) : locations.p1_end;
				serial_data.erase(serial_data_begin, erase_end);
				buffer_was_mutated = true;
			}
			else
			{
				// Buffer seems okay...continue and process it.
			}
		}

		return buffer_was_mutated;
	}

}
// namespace AqualinkAutomate::Generators
