#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/light/light_message.h"

namespace AqualinkAutomate::Messages
{

	enum class LightStates : uint8_t
	{
		Off = 0x00,
		On = 0x01,
		Unknown = 0xFF
	};

	// LightMessage_Status models the status reply from a Jandy colored-light
	// controller (device ids 0xF0-0xF4).
	//
	// Wire layout (absolute offsets into the full framed packet; payload starts
	// at Index_DataStart == 4):
	//
	//   [DLE STX][dest=0xF0..0xF4][cmd=Light_Status][state][mode][cksum][DLE ETX]
	//
	//   * state (byte 4): 0x00 = off, 0x01 = on.
	//   * mode  (byte 5): active colour / light-show index (0 when off).
	//
	// AqualinkD documents only the device-id range for these controllers; the
	// exact colour-mode encoding is not otherwise published, so 'mode' is kept
	// as an opaque raw value that callers can interpret once a live capture is
	// available.
	class LightMessage_Status : public LightMessage, public Interfaces::IMessageSignalRecv<LightMessage_Status>
	{
	public:
		static const uint8_t Index_State = 4;
		static const uint8_t Index_Mode = 5;

	public:
		LightMessage_Status() noexcept;
		~LightMessage_Status() override = default;

		LightStates State() const;
		bool IsOn() const;
		uint8_t LightMode() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		LightStates m_State{ LightStates::Unknown };
		uint8_t m_LightMode{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
