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

	class IAQMessage_TitleMessage : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_TitleMessage>
	{
	public:
		static constexpr uint8_t Index_TitleText = 4;

	public:
		IAQMessage_TitleMessage() noexcept;
		~IAQMessage_TitleMessage() override = default;
		std::string Title() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::string m_Title;
	};

}
// namespace AqualinkAutomate::Messages
