#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_PageMessage : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_PageMessage>
	{
	public:
		static constexpr uint8_t Index_LineId = 4;
		static constexpr uint8_t Index_LineText = 5;

	public:
		IAQMessage_PageMessage() noexcept;
		~IAQMessage_PageMessage() override = default;
		uint8_t LineId() const;
		std::string Line() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_LineId{ 0 };
		std::string m_Line;
	};

}
// namespace AqualinkAutomate::Messages
