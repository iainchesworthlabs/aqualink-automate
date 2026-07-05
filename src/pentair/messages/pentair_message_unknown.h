#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/pentair_message.h"

namespace AqualinkAutomate::Pentair::Messages
{

	// Catch-all for any Pentair frame whose CMD byte does not map to a known
	// PentairMessageIds value.  The frame is still fully validated (framing +
	// checksum) and counted; the payload is retained verbatim for diagnostics.
	class PentairMessage_Unknown : public PentairMessage, public Interfaces::IMessageSignalRecv<PentairMessage_Unknown>
	{
	public:
		PentairMessage_Unknown() noexcept;
		~PentairMessage_Unknown() override = default;

		const std::vector<uint8_t>& Payload() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::vector<uint8_t> m_Payload;
	};

}
// namespace AqualinkAutomate::Pentair::Messages
