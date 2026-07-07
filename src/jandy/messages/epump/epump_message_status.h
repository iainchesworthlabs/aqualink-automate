#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/epump/epump_message.h"

namespace AqualinkAutomate::Messages
{

	class EPumpMessage_Status : public EPumpMessage, public Interfaces::IMessageSignalRecv<EPumpMessage_Status>
	{
	public:
		static const uint8_t Index_SubCommand = 4;

	public:
		EPumpMessage_Status() noexcept;
		~EPumpMessage_Status() override = default;
		uint8_t SubCommand() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_SubCommand{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
