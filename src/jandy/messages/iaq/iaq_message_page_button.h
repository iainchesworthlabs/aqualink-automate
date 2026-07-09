#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <string>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{
	enum class ButtonStatuses : uint8_t
	{
		Off = 0x00,
		On = 0x01,
		Enabled = 0x02,
		EnabledStandby = 0x03,
		Unknown = 0xFF
	};

	enum class ButtonTypes : uint8_t
	{
		None = 0x00,
		Generic = 0x01,
		Cooling = 0x02,
		Waterfall = 0x03,
		Fountain = 0x04,
		Heating = 0x05,
		HeatingAndCooling = 0x06,
		Light = 0x07,
		Filtration = 0x08,
		SolarHeating = 0x09,
		DeckJet = 0x0A,
		HeaterGreen = 0x0B,
		Unknown = 0xFF
	};

	class IAQMessage_PageButton : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_PageButton>
	{
	public:
		static constexpr uint8_t Index_ButtonIndex = 4;
		static constexpr uint8_t Index_ButtonState = 5;
		// Unknown byte value stored at index position 6.
		static constexpr uint8_t Index_ButtonType = 7;
		static constexpr uint8_t Index_ButtonNameText = 8;

	public:
		IAQMessage_PageButton() noexcept;
		~IAQMessage_PageButton() override = default;
		uint8_t ButtonIndex() const;
		ButtonStatuses ButtonStatus() const;
		ButtonTypes ButtonType() const;
		std::string ButtonName() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		uint8_t m_ButtonIndex{ 0 };
		ButtonStatuses m_ButtonStatus{ ButtonStatuses::Unknown };
		ButtonTypes m_ButtonType{ ButtonTypes::Unknown };
		std::string m_ButtonName;
	};

}
// namespace AqualinkAutomate::Messages
