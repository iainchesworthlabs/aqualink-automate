#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
//#include "factories/jandy_message_factory_registration.h"
#include "messages/jandy_message.h"

namespace AqualinkAutomate::Messages
{

	class JandyMessage_MessageLong : public JandyMessage, public Interfaces::IMessageSignalRecv<JandyMessage_MessageLong>
	{
		static const uint8_t DISPLAY_LINE_LENGTH = 16;        // Visible characters per display line.
	static const uint8_t MAXIMUM_MESSAGE_LENGTH = DISPLAY_LINE_LENGTH + 1; // Plus a NUL terminator character.

		static const uint8_t Index_LineId = 4;
		static const uint8_t Index_LineText = 5;

	public:
		JandyMessage_MessageLong() noexcept;
		JandyMessage_MessageLong(const uint8_t line_id, const std::string& line);
		~JandyMessage_MessageLong() override = default;
		uint8_t LineId() const;
		std::string Line() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_LineId;
		std::string m_Line;
	};

}
// namespace AqualinkAutomate::Messages
