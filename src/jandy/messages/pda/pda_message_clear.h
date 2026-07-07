#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pda/pda_message.h"

namespace AqualinkAutomate::Messages
{

	class PDAMessage_Clear : public PDAMessage, public Interfaces::IMessageSignalRecv<PDAMessage_Clear>
	{
	public:
		PDAMessage_Clear() noexcept;
		~PDAMessage_Clear() override = default;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;
	};

}
// namespace AqualinkAutomate::Messages
