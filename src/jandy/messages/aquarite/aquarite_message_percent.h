#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/aquarite/aquarite_message.h"

namespace AqualinkAutomate::Messages
{

	class AquariteMessage_Percent : public AquariteMessage, public Interfaces::IMessageSignalRecv<AquariteMessage_Percent>
	{
	public:
		static const uint8_t Index_Percent = 4;
		static const uint8_t Value_Boost = 101;
		static const uint8_t Value_ServiceMode = 0xFF;

	public:
		AquariteMessage_Percent() noexcept;
		~AquariteMessage_Percent() override = default;

		uint8_t GeneratingPercentage() const;
		bool IsBoostMode() const;
		bool IsServiceMode() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_Percent{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
