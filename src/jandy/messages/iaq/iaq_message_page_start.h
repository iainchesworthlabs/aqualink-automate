#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_PageStart : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_PageStart>
	{
	public:
		// The page identifier rides as the first payload byte (e.g. 0x01 home, 0x14 Setup,
		// 0x3a Spa Remotes, 0x3b the 4-Function detail). Used to page-GATE the spa-switch writer.
		static constexpr uint8_t Index_PageId = 4;

	public:
		IAQMessage_PageStart() noexcept;
		~IAQMessage_PageStart() override = default;

	public:
		uint8_t PageId() const;

	public:
		std::string ToString() const override;

	public:
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_PageId{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
