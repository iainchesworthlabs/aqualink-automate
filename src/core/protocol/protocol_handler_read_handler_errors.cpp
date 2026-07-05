#include <format>

#include "errors/message_errors.h"
#include "errors/protocol_errors.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "profiling/factories/profiler_factory.h"
#include "protocol/protocol_handler_read.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Protocol
{

	void ProtocolHandler_ReadOp_ErrorHandler(ProtocolErrorCode error_code, const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub)
	{
		// Compare the WHOLE error_code (value AND category), not just value().
		// The Message and Protocol categories use disjoint numeric ranges today,
		// but matching value() alone would silently misroute a future 2001 from an
		// unrelated category.  A small if-chain over typed codes keeps the dispatch
		// category-aware and avoids a single untyped switch over raw integers.

		if (error_code == make_error_code(ErrorCodes::Message_ErrorCodes::Error_CannotFindGenerator))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> cannot find generator for message type");
			if (statistics_hub) { ++statistics_hub->MessageErrors.GeneratorFailures; }
		}
		else if (error_code == make_error_code(ErrorCodes::Message_ErrorCodes::Error_GeneratorFailed))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> generator failed to create message");
			if (statistics_hub) { ++statistics_hub->MessageErrors.GeneratorFailures; }
		}
		else if (error_code == make_error_code(ErrorCodes::Message_ErrorCodes::Error_FailedToDeserialize))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> failed to deserialize bytes");
			Factory::ProfilerFactory::Instance().Get()->Message("Protocol: Deserialization failure", static_cast<uint32_t>(UnitColours::Red));
			if (statistics_hub) { ++statistics_hub->MessageErrors.DeserializationFailures; }
		}
		else if (error_code == make_error_code(ErrorCodes::Protocol_ErrorCodes::InvalidPacketFormat))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> invalid packet format");
			Factory::ProfilerFactory::Instance().Get()->Message("Protocol: Invalid packet format", static_cast<uint32_t>(UnitColours::Red));
			if (statistics_hub) { ++statistics_hub->MessageErrors.InvalidPacketFormat; }
		}
		else if (error_code == make_error_code(ErrorCodes::Protocol_ErrorCodes::ChecksumFailure))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> checksum failure");
			Factory::ProfilerFactory::Instance().Get()->Message("Protocol: Checksum failure", static_cast<uint32_t>(UnitColours::Red));
			if (statistics_hub) { ++statistics_hub->MessageErrors.ChecksumFailures; }
		}
		else if (error_code == make_error_code(ErrorCodes::Protocol_ErrorCodes::OverlappingPackets))
		{
			LogDebug(Channel::Protocol, "Protocol error while processing messages -> overlapping packets detected");
			if (statistics_hub) { ++statistics_hub->MessageErrors.OverlappingPackets; }
		}
		else if (error_code == make_error_code(ErrorCodes::Protocol_ErrorCodes::DataAvailableToProcess)
			|| error_code == make_error_code(ErrorCodes::Protocol_ErrorCodes::WaitingForMoreData))
		{
			// CONTROL FLOW, not a failure.  These are the most frequent results on a
			// healthy bus (a partially-buffered or not-yet-mine frame), so this path
			// must NOT eagerly format a string when the Trace channel is filtered
			// out, and must NOT bump any error metric.
			LogTrace(Channel::Protocol, [&error_code] { return std::format("Protocol processing paused: {}", error_code.message()); });
		}
		else
		{
			LogDebug(Channel::Protocol, [&error_code] { return std::format("Protocol error while processing messages -> unknown error occured: {}, {}", error_code.value(), error_code.message()); });
		}
	}

}
// namespace AqualinkAutomate::Protocol
