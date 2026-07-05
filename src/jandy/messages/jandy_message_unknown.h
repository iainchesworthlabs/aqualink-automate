#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/jandy_message.h"

namespace AqualinkAutomate::Messages
{

	class JandyMessage_Unknown : public JandyMessage, public Interfaces::IMessageSignalRecv<JandyMessage_Unknown>
	{
	public:
		JandyMessage_Unknown() noexcept;
		~JandyMessage_Unknown() override = default;

	public:
		std::string ToString() const override;

	public:
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

		// Raw payload bytes retained verbatim from the wire (between the 4-byte header
		// and 3-byte footer). Empty until DeserializeContents runs. Retained so that
		// undecoded frames -- e.g. unrecognised commands sent TO the master -- can be
		// inspected byte-for-byte by diagnostics tooling.
		const std::vector<uint8_t>& Payload() const;

	private:
		std::vector<uint8_t> m_Payload;
	};

}
// namespace AqualinkAutomate::Messages
