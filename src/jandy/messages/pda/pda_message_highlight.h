#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pda/pda_message.h"

namespace AqualinkAutomate::Messages
{

	class PDAMessage_Highlight : public PDAMessage, public Interfaces::IMessageSignalRecv<PDAMessage_Highlight>
	{
	public:
		static const uint8_t Index_LineId = 4;

	public:
		PDAMessage_Highlight() noexcept;
		explicit PDAMessage_Highlight(const uint8_t line_id);
		~PDAMessage_Highlight() override = default;

		uint8_t LineId() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_LineId;
	};

}
// namespace AqualinkAutomate::Messages
