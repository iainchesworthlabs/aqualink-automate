#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "interfaces/imessagesignal_send.h"
#include "messages/jandy_message.h"

namespace AqualinkAutomate::Messages
{

	enum class AckTypes : uint8_t
	{
		ACK_IAQTouch = 0x00,
		ACK_PDA = 0x40,		

		// First Generation responses.
		V1_Normal = 0x00,
		V1_ScreenBusy_Scroll = 0x01,
		V1_ScreenBusy_Block = 0x03,

		// Second Generation responses.
		V2_Normal = 0x80,
		V2_ScreenBusy_Scroll = 0x81,
		V2_ScreenBusy_Block = 0x83,

		Unknown_3F = 0x3F,
		Unknown_70 = 0x70,
		Unknown_72 = 0x72,
		Unknown = 0xFF
	};

	class JandyMessage_Ack : public JandyMessage, public Interfaces::IMessageSignalRecv<JandyMessage_Ack>, public Interfaces::IMessageSignalSend<JandyMessage_Ack>
	{
		static const uint8_t AQUALINK_MASTER_ID = 0x00;

		static const uint8_t Index_AckType = 4;
		static const uint8_t Index_Command = 5;

	public:
		static constexpr JandyMessageIds ID = JandyMessageIds::Ack;

	public:
		JandyMessage_Ack() noexcept;
		JandyMessage_Ack(AckTypes ack_type, uint8_t command);
		explicit JandyMessage_Ack(uint8_t ack_value, uint8_t command);
		~JandyMessage_Ack() override = default;
		AckTypes AckType() const;
		uint8_t Command() const;
		template<typename COMMAND_TYPE>
		COMMAND_TYPE Command(std::function<COMMAND_TYPE(uint8_t)> command_decoder) const
		{
			return command_decoder(Command());
		}
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_AckType;
		uint8_t m_Command;
	};

}
// namespace AqualinkAutomate::Messages
