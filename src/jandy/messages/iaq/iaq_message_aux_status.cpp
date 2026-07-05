#include <format>
#include <span>

#include "messages/iaq/iaq_message_aux_status.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_text_helpers.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	IAQMessage_AuxStatus::IAQMessage_AuxStatus() noexcept :
		IAQMessage(JandyMessageIds::IAQ_AuxStatus),
		Interfaces::IMessageSignalRecv<IAQMessage_AuxStatus>()
	{
	}


	const std::vector<uint8_t>& IAQMessage_AuxStatus::RawPayload() const
	{
		return m_RawPayload;
	}

	const std::vector<AuxDeviceInfo>& IAQMessage_AuxStatus::Devices() const
	{
		return m_Devices;
	}

	uint8_t IAQMessage_AuxStatus::DeviceCount() const
	{
		return static_cast<uint8_t>(m_Devices.size());
	}

	std::string IAQMessage_AuxStatus::ToString() const
	{
		std::string devices_str;
		for (size_t i = 0; i < m_Devices.size(); ++i)
		{
			if (i > 0)
			{
				devices_str += ", ";
			}
			devices_str += std::format("{}({})", m_Devices[i].name, m_Devices[i].is_on ? "ON" : "OFF");
		}
		return std::format("Packet: {} || Devices: {} [{}]", IAQMessage::ToString(), m_Devices.size(), devices_str);
	}

	bool IAQMessage_AuxStatus::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		return false;
	}

	bool IAQMessage_AuxStatus::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, [&message_bytes]() { return std::format("Deserialising {} bytes from span into IAQMessage_AuxStatus type", message_bytes.size()); });

		if (message_bytes.size() <= JandyMessage::PACKET_HEADER_LENGTH + JandyMessage::PACKET_FOOTER_LENGTH)
		{
			LogDebug(Channel::Messages, "IAQMessage_AuxStatus is too short to contain any payload.");
			return false;
		}

		// Extract raw payload (between the header and the footer) for backward compatibility.
		m_RawPayload.assign(
			message_bytes.begin() + JandyMessage::PACKET_HEADER_LENGTH,
			message_bytes.end() - JandyMessage::PACKET_FOOTER_LENGTH);

		const auto& payload = m_RawPayload;
		const size_t payload_size = payload.size();
		size_t pos = 0;

		// Byte 0: num_devices
		if (pos >= payload_size)
		{
			LogDebug(Channel::Messages, "IAQMessage_AuxStatus payload too short for device count.");
			return false;
		}

		const uint8_t num_devices = payload[pos++];

		// Read num_devices device index bytes
		if (pos + num_devices > payload_size)
		{
			LogDebug(Channel::Messages, "IAQMessage_AuxStatus payload too short for device indices.");
			return false;
		}

		std::vector<uint8_t> device_indices(payload.begin() + static_cast<ptrdiff_t>(pos), payload.begin() + static_cast<ptrdiff_t>(pos + num_devices));
		pos += num_devices;

		// Parse each device: status(1), type(1), pad(2), name_len(1), name(name_len)
		m_Devices.clear();
		m_Devices.reserve(num_devices);

		for (uint8_t i = 0; i < num_devices; ++i)
		{
			// Need at least 5 bytes: status(1) + type(1) + pad(2) + name_len(1)
			if (pos + 5 > payload_size)
			{
				LogDebug(Channel::Messages, [i]() { return std::format("IAQMessage_AuxStatus payload too short for device {} header.", i); });
				return false;
			}

			AuxDeviceInfo info;
			info.device_index = device_indices[i];
			info.is_on = (payload[pos] == 0x01);
			pos++;

			info.type = payload[pos];
			pos++;

			// Skip 2 padding bytes
			pos += 2;

			const uint8_t name_len = payload[pos];
			pos++;

			if (pos + name_len > payload_size)
			{
				LogDebug(Channel::Messages, [i, name_len]() { return std::format("IAQMessage_AuxStatus payload too short for device {} name (need {} bytes).", i, name_len); });
				return false;
			}

			// Sanitise wire-sourced aux device name to printable ASCII before it is
			// copied into the label/UI (control / escape bytes would otherwise pass through).
			Text::AppendSanitisedAscii(info.name, std::span<const uint8_t>(payload).subspan(pos, name_len));
			pos += name_len;

			m_Devices.push_back(std::move(info));
		}

		LogDebug(Channel::Messages, [this]() { return std::format("Deserialised IAQMessage_AuxStatus: {} devices", m_Devices.size()); });

		return true;
	}

}
// namespace AqualinkAutomate::Messages
