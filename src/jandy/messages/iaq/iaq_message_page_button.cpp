#include <algorithm>
#include <cstddef>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/iaq/iaq_message_page_button.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	IAQMessage_PageButton::IAQMessage_PageButton() noexcept :
		IAQMessage(JandyMessageIds::IAQ_PageButton),
		Interfaces::IMessageSignalRecv<IAQMessage_PageButton>()
	{
	}


	uint8_t IAQMessage_PageButton::ButtonIndex() const
	{
		return m_ButtonIndex;
	}

	ButtonStatuses IAQMessage_PageButton::ButtonStatus() const
	{
		return m_ButtonStatus;
	}

	ButtonTypes IAQMessage_PageButton::ButtonType() const
	{
		return m_ButtonType;
	}

	std::string IAQMessage_PageButton::ButtonName() const
	{
		return m_ButtonName;
	}

	std::string IAQMessage_PageButton::ToString() const
	{
		return std::format(
			"Packet: {} || Payload: Index: {}, Status: {}, Type: {}, Name: '{}'",
			IAQMessage::ToString(),
			m_ButtonIndex,
			magic_enum::enum_name(m_ButtonStatus),
			magic_enum::enum_name(m_ButtonType),
			m_ButtonName);
	}

	bool IAQMessage_PageButton::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool IAQMessage_PageButton::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into IAQMessage_PageButton type", message_bytes.size()); });

		if (!Text::RequireIndex(message_bytes, Index_ButtonState, "IAQMessage_PageButton", "ButtonState"))
		{
			return false;
		}

		if (!Text::RequireIndex(message_bytes, Index_ButtonType, "IAQMessage_PageButton", "ButtonType"))
		{
			return false;
		}

		// The (optional) ButtonName payload runs from Index_ButtonNameText up to the
		// 3-byte footer.  Require at least that the footer is present so the name
		// length can never underflow; an empty name is a valid result.
		if (message_bytes.size() < Index_ButtonNameText + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "IAQMessage_PageButton is too short for content extraction");
			return false;
		}

		m_ButtonIndex = Text::ReadU8(message_bytes, Index_ButtonIndex);
		m_ButtonStatus = magic_enum::enum_cast<ButtonStatuses>(Text::ReadU8(message_bytes, Index_ButtonState)).value_or(ButtonStatuses::Unknown);
		m_ButtonType = magic_enum::enum_cast<ButtonTypes>(Text::ReadU8(message_bytes, Index_ButtonType)).value_or(ButtonTypes::Unknown);

		// The button label (and its trailing state text) is a display line: an interior
		// NUL separates the two fields ("Pool Heat" \0 "OFF") and becomes a space rather
		// than a '?'; trailing NUL pad is stripped.
		m_ButtonName = Text::ExtractTrailingDisplayLine(message_bytes, Index_ButtonNameText);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
