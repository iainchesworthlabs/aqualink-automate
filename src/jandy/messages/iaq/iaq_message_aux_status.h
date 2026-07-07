#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	struct AuxDeviceInfo
	{
		uint8_t device_index{0};
		bool is_on{false};
		uint8_t type{0};
		std::string name;
	};

	class IAQMessage_AuxStatus : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_AuxStatus>
	{
	public:
		IAQMessage_AuxStatus() noexcept;
		~IAQMessage_AuxStatus() override = default;
		const std::vector<uint8_t>& RawPayload() const;
		const std::vector<AuxDeviceInfo>& Devices() const;
		uint8_t DeviceCount() const;
		std::string ToString() const override;
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::vector<uint8_t> m_RawPayload;
		std::vector<AuxDeviceInfo> m_Devices;
	};

}
// namespace AqualinkAutomate::Messages
