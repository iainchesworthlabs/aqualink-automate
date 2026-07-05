#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "interfaces/imessagesignal_send.h"
#include "messages/pentair_message.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// Set chlorinator output command (CMD 0x11), sent controller -> chlorinator.
	// The DATA section is a single byte: the requested output percentage (0-100).
	class PentairChlorinatorMessage_SetOutput :
		public PentairMessage,
		public Interfaces::IMessageSignalRecv<PentairChlorinatorMessage_SetOutput>,
		public Interfaces::IMessageSignalSend<PentairChlorinatorMessage_SetOutput>
	{
	public:
		static constexpr uint8_t Data_Index_Output = 0;

	public:
		PentairChlorinatorMessage_SetOutput() noexcept;
		PentairChlorinatorMessage_SetOutput(uint8_t from, uint8_t destination, uint8_t output_percent) noexcept;
		~PentairChlorinatorMessage_SetOutput() override = default;

		uint8_t OutputPercent() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_OutputPercent;
	};

}
// namespace AqualinkAutomate::Pentair::Messages
