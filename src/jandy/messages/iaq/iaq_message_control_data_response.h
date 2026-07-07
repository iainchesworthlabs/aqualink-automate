#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_send.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_ControlDataResponse : public IAQMessage, public Interfaces::IMessageSignalSend<IAQMessage_ControlDataResponse>
	{
		static constexpr size_t DATA_PAYLOAD_SIZE = 17;

	public:
		explicit IAQMessage_ControlDataResponse(const std::string& ascii_data);
		~IAQMessage_ControlDataResponse() override = default;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::string m_AsciiData;
	};

}
// namespace AqualinkAutomate::Messages
