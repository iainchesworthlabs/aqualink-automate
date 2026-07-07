#pragma once

#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/aquarite/aquarite_message.h"

namespace AqualinkAutomate::Messages
{

	enum class PanelDataTypes : uint8_t
	{
		PanelRevision = 0x01,
		PanelType = 0x02,
		Unknown = 0xFF
	};

	class AquariteMessage_GetId : public AquariteMessage, public Interfaces::IMessageSignalRecv<AquariteMessage_GetId>
	{
	public:
		static const uint8_t Index_RequestedDataFlag = 4;

	public:
		AquariteMessage_GetId() noexcept;
		explicit AquariteMessage_GetId(PanelDataTypes requested_panel_data);
		~AquariteMessage_GetId() override = default;

		PanelDataTypes RequestedPanelData() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		PanelDataTypes m_RequestedPanelData;
	};

}
// namespace AqualinkAutomate::Messages
