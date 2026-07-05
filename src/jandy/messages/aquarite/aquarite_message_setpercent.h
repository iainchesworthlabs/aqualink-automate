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

	// AQUARITE_SetPercent (0x15) -- a second "set output %" command that the AquaRite
	// firmware accepts on an identical code path to AQUARITE_Percent (0x11) (recovered
	// from the official Jandy RS simulator; see docs/alwin32/). The distinction from
	// 0x11 (e.g. pool vs spa output, or AquaPure vs AquaRite) is not yet confirmed --
	// pending a live capture. Modelled as a percent message so the value is decoded and
	// logged rather than dropped to Unknown; no device-side behaviour is wired to it yet.
	class AquariteMessage_SetPercent : public AquariteMessage, public Interfaces::IMessageSignalRecv<AquariteMessage_SetPercent>
	{
	public:
		static const uint8_t Index_Percent = 4;
		static const uint8_t Value_Boost = 101;
		static const uint8_t Value_ServiceMode = 0xFF;

	public:
		AquariteMessage_SetPercent() noexcept;
		~AquariteMessage_SetPercent() override = default;

		uint8_t GeneratingPercentage() const;
		bool IsBoostMode() const;
		bool IsServiceMode() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_Percent{ 0 };
	};

}
// namespace AqualinkAutomate::Messages
