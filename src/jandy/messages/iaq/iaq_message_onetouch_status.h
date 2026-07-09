#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_OneTouchStatus : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_OneTouchStatus>
	{
	public:
		IAQMessage_OneTouchStatus() noexcept;
		~IAQMessage_OneTouchStatus() override = default;
		const std::vector<uint8_t>& RawPayload() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::vector<uint8_t> m_RawPayload;
	};

}
// namespace AqualinkAutomate::Messages
