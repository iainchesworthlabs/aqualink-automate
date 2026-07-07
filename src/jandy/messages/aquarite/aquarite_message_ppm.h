#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/aquarite/aquarite_message.h"

namespace AqualinkAutomate::Messages
{

	enum class AquariteStatuses : uint8_t
	{
		On = 0x00,
		TurningOff = 0x09,
		Off = 0xFF,
		Warning_NoFlow = 0x01,
		Warning_LowSalt = 0x02,
		Warning_HighSalt = 0x04,
		Warning_HighCurrent = 0x10,
		Warning_CleanCell = 0x08,
		Warning_LowVoltage = 0x20,
		Warning_LowTemperature = 0x40,
		Error_CheckPCB = 0x80,
		GeneralFault = 0xFD,
		Unknown = 0xFE
	};

	class AquariteMessage_PPM : public AquariteMessage, public Interfaces::IMessageSignalRecv<AquariteMessage_PPM>
	{
	public:
		static const uint8_t Index_PPM = 4;
		static const uint8_t Index_Status = 5;

	public:
		AquariteMessage_PPM() noexcept;
		~AquariteMessage_PPM() override = default;

		uint16_t SaltConcentrationPPM() const;
		AquariteStatuses Status() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint16_t m_PPM{ 0 };
		AquariteStatuses m_Status{ AquariteStatuses::Unknown };
	};

}
// namespace AqualinkAutomate::Messages
