#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "messages/jandy_message.h"
#include "messages/jandy_message_ids.h"

namespace AqualinkAutomate::Messages
{
	class SerialAdapterMessage : public JandyMessage
	{
	public:
		explicit SerialAdapterMessage(const JandyMessageIds& msg_id);
		~SerialAdapterMessage() override = default;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override = 0;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override = 0;
	};

}
// namespace AqualinkAutomate::Messages
