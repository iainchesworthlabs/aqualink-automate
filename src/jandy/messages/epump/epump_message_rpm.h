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

	// ePump 0x44/0x45 model -- UNRESOLVED, capture-gated (see docs/alwin32/epump.md).
	// The official Jandy RS simulator decodes 0x44 ('D') and 0x45 ('E') as master->pump
	// COMMANDS -- 'D' = set speed (RPM x4, little-endian, in the command payload) and
	// 'E' = query a value (a selector byte chooses RPM vs Watts) -- alongside 'A'(0x41)
	// poll, 'B'(0x42) stop, 'C'(0x43) start. This class instead models 0x44 as a pump
	// RPM *report*. The sim does not cleanly define the pump's reply command byte (it
	// echoes the inbound frame) and its value sits ~1 byte later than read here, so the
	// sim neither confirms nor cleanly replaces this decode; and the project's captures
	// contain no ePump traffic. The two models are therefore left as-is pending a live
	// ePump capture rather than flipped on incomplete sim evidence.
	class EPumpMessage_RPM : public EPumpMessage, public Interfaces::IMessageSignalRecv<EPumpMessage_RPM>
	{
	public:
		static const uint8_t Index_RPM_Low = 6;
		static const uint8_t Index_RPM_High = 7;

	public:
		EPumpMessage_RPM() noexcept;
		~EPumpMessage_RPM() override = default;
		uint16_t RPM() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint16_t m_RPM{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
