#include <format>

#include "interfaces/imessagesignal_recv.h"
#include "kernel/statistics_hub.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "protocol/protocol_handler_read.h"
#include "protocol/protocol_message_types.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Protocol
{

	void ProtocolHandler_ReadOp_MessageHandler(const ProtocolMessagePtr& wrapper, [[maybe_unused]] const std::shared_ptr<Kernel::StatisticsHub>& statistics_hub)
	{
		LogTrace(Channel::Protocol, "Message received; emitting signal to listening consumer slots.");

		if (!wrapper)
		{
			LogDebug(Channel::Protocol, "Null message wrapper when attempting to signal consumer slots; cannot process");
		}
		else if (auto* signal_base = wrapper.GetSignalInterface(); nullptr == signal_base)
		{
			LogDebug(Channel::Protocol, "Message does not implement IMessageSignalRecvBase interface; cannot process");
		}
		else
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ProtocolHandler::HandleRead -> signalling", std::source_location::current());

			// Process the message...as per protocol requirements
			signal_base->Signal_MessageWasReceived();
		}
	}

}
// namespace AqualinkAutomate::Protocol

