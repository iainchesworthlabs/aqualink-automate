#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	// IAQ_TableMessage (0x26) carries TWO leading bytes before the line text -- a line
	// index and an attribute byte -- unlike PageMessage (0x25), which has a single
	// leading line byte. Recovered from the official Jandy RS simulator's page builder
	// (FUN_004556c7 writes [line][attr][text...] at payload offsets 4/5/6; see
	// docs/alwin32/pwrcntr-behavior.md). The project's real captures do not contain any
	// 0x26 frame, so this is the best available (vendor-firmware) layout; the exact
	// meaning of the attribute byte is not yet pinned.
	class IAQMessage_TableMessage : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_TableMessage>
	{
	public:
		static const uint8_t Index_LineId = 4;
		static const uint8_t Index_Attribute = 5;
		static const uint8_t Index_LineText = 6;

	public:
		IAQMessage_TableMessage() noexcept;
		~IAQMessage_TableMessage() override = default;

	public:
		uint8_t LineId() const;
		uint8_t Attribute() const;
		std::string Line() const;

	public:
		std::string ToString() const override;

	public:
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_LineId;
		uint8_t m_Attribute{ 0 };
		std::string m_Line;
	};

}
// namespace AqualinkAutomate::Messages
