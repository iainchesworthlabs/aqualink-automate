#include "errors/protocol_errors.h"

namespace AqualinkAutomate::ErrorCodes
{
	const Protocol_ErrorCategory& Protocol_ErrorCategory::Instance()
	{
		static Protocol_ErrorCategory category;
		return category;
	}

	std::string_view Protocol_ErrorCategory::Describe(Protocol_ErrorCodes e)
	{
		using enum Protocol_ErrorCodes;

		switch (e)
		{
		case DataAvailableToProcess:
			return "A complete frame is available in the buffer and ready to process";

		case WaitingForMoreData:
			return "Not enough data has been received yet to decode a frame";

		case InvalidPacketFormat:
			return "The packet structure does not match the expected protocol format";

		case UnknownFailure:
			return "An unspecified protocol processing failure occurred";

		case ChecksumFailure:
			return "The packet checksum did not match the computed value";

		case OverlappingPackets:
			return "Overlapping packet boundaries were detected in the buffer";

		default:
			return "Unknown protocol error";
		}
	}

}
// namespace AqualinkAutomate::ErrorCodes

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Protocol_ErrorCategory::MakeErrorCode(e);
}

boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Protocol_ErrorCategory::MakeErrorCondition(e);
}
