#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/jandy_message.h"

namespace AqualinkAutomate::Messages
{

	class JandyMessage_Message : public JandyMessage, public Interfaces::IMessageSignalRecv<JandyMessage_Message>
	{
		static const uint8_t DISPLAY_LINE_LENGTH = 16;        // Visible characters per display line.
	static const uint8_t MAXIMUM_MESSAGE_LENGTH = DISPLAY_LINE_LENGTH + 1; // Plus a NUL terminator character.

		static const uint8_t Index_LineText = 4;

	public:
		JandyMessage_Message() noexcept;
		explicit JandyMessage_Message(const std::string& line);
		~JandyMessage_Message() override = default;

		std::string Line() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::string m_Line;
	};

}
// namespace AqualinkAutomate::Messages
