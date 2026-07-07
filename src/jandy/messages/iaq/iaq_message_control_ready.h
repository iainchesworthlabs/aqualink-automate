#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_ControlReady : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_ControlReady>
	{
	public:
		IAQMessage_ControlReady() noexcept;
		~IAQMessage_ControlReady() override = default;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;
	};

}
// namespace AqualinkAutomate::Messages
