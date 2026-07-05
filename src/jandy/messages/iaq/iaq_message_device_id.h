#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	// iAqualink2 boot-init message, wire cmd 0x51 (master -> iAqualink2 0xA0-0xA3). The payload
	// is a NUL-terminated ASCII string, observed as a 16-char hex value e.g. "1BA62825B7C69A4C".
	//
	// The STRUCTURE (a NUL-terminated ASCII id) is decoded straight from the wire and is solid.
	// The NAME ("DeviceId" / serial-identity) is a WORKING HYPOTHESIS inferred from the content
	// alone -- there is no authoritative source: it is undocumented in the Jandy spec, and
	// AqualinkD (itself reverse-engineered) does not decode it either (logs it "Unknown"). Treat
	// the field meaning as tentative until confirmed against more captures / another unit.
	// See docs/iaqualink2_init_handshake.md.
	class IAQMessage_DeviceId : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_DeviceId>
	{
	public:
		IAQMessage_DeviceId() noexcept;
		~IAQMessage_DeviceId() override = default;

		// The decoded NUL-terminated ASCII device id (empty until deserialised).
		const std::string& DeviceId() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::string m_DeviceId;
	};

}
// namespace AqualinkAutomate::Messages
