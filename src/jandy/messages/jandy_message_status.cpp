#include <format>

#include <magic_enum/magic_enum.hpp>

#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_status.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{
	const uint8_t JandyMessage_Status::STATUS_PAYLOAD_LENGTH{ 5 };

	JandyMessage_Status::JandyMessage_Status() noexcept :
		JandyMessage(JandyMessageIds::Status),
		Interfaces::IMessageSignalRecv<JandyMessage_Status>() 
	{
	}


	ComboModes JandyMessage_Status::Mode() const
	{
		return m_Mode;
	}

	Kernel::PumpStatuses JandyMessage_Status::FilterPump() const
	{
		return m_FilterPump;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux1() const
	{
		return m_Aux1;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux2() const
	{
		return m_Aux2;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux3() const
	{
		return m_Aux3;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux4() const
	{
		return m_Aux4;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux5() const
	{
		return m_Aux5;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux6() const
	{
		return m_Aux6;
	}

	Kernel::AuxillaryStatuses JandyMessage_Status::Aux7() const
	{
		return m_Aux7;
	}

	Kernel::HeaterStatuses JandyMessage_Status::PoolHeater() const
	{
		return m_PoolHeater;
	}

	Kernel::HeaterStatuses JandyMessage_Status::SpaHeater() const
	{
		return m_SpaHeater;
	}

	Kernel::HeaterStatuses JandyMessage_Status::SolarHeater() const
	{
		return m_SolarHeater;
	}

	const std::vector<uint8_t>& JandyMessage_Status::RawPayload() const
	{
		return m_RawPayload;
	}

	std::string JandyMessage_Status::ToString() const
	{
		return std::format("Packet: {} || Payload: {}", JandyMessage::ToString(), 0);
	}

	bool JandyMessage_Status::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		LogTrace(Channel::Messages, std::format("Serialising JandyMessage_Status type into {} bytes", message_bytes.size()));

		// Insert 5 NUL bytes to ensure that the payload is the correct length.
		// Note that there is no actual computation of the correct content here.

		message_bytes.push_back(0x00);
		message_bytes.push_back(0x00);
		message_bytes.push_back(0x00);
		message_bytes.push_back(0x00);
		message_bytes.push_back(0x00);

		return true;
	}

	bool JandyMessage_Status::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes into JandyMessage_Status type", message_bytes.size()));

		// Retain the raw payload verbatim (the bytes between the 4-byte header and 3-byte footer)
		// so a handler can read payload byte [0] (wire index 4), which the structured Aux*/Heater
		// decode below does not cover. Used by the spa-side remote's LED-image decode.
		m_RawPayload.clear();
		if (message_bytes.size() >= static_cast<std::size_t>(Index_DataStart) + PACKET_FOOTER_LENGTH)
		{
			const auto payload = message_bytes.subspan(Index_DataStart, message_bytes.size() - Index_DataStart - PACKET_FOOTER_LENGTH);
			m_RawPayload.assign(payload.begin(), payload.end());
		}

		if (message_bytes.size() < static_cast<uint64_t>(JandyMessage::MINIMUM_PACKET_LENGTH + STATUS_PAYLOAD_LENGTH))
		{
			LogWarning(Channel::Messages, std::format("Failed during JandyMessage_Status deserialising; payload size mismatch: {} vs {}", STATUS_PAYLOAD_LENGTH, message_bytes.size()));
		}
		else
		{
			LogTrace(Channel::Messages, std::format("Deserialising {} payload bytes from span into JandyMessage_Status type", message_bytes.size()));

			m_Aux5 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[5] & 0x80) >> 7).value_or(Kernel::AuxillaryStatuses::Unknown);
			m_Aux2 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[5] & 0x40) >> 6).value_or(Kernel::AuxillaryStatuses::Unknown);
			m_Aux3 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[5] & 0x10) >> 4).value_or(Kernel::AuxillaryStatuses::Unknown);
			m_Aux7 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[5] & 0x01) >> 0).value_or(Kernel::AuxillaryStatuses::Unknown);

			m_FilterPump = magic_enum::enum_cast<Kernel::PumpStatuses>((message_bytes[6] & 0x10) >> 4).value_or(Kernel::PumpStatuses::Unknown);
			m_Mode = magic_enum::enum_cast<ComboModes>((message_bytes[6] & 0x04) >> 2).value_or(ComboModes::Unknown);
			m_Aux1 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[6] & 0x01) >> 0).value_or(Kernel::AuxillaryStatuses::Unknown);

			m_Aux6 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[7] & 0x40) >> 6).value_or(Kernel::AuxillaryStatuses::Unknown);
			m_Aux4 = magic_enum::enum_cast<Kernel::AuxillaryStatuses>((message_bytes[7] & 0x01) >> 0).value_or(Kernel::AuxillaryStatuses::Unknown);

			m_PoolHeater = magic_enum::enum_cast<Kernel::HeaterStatuses>((message_bytes[8] & 0x70) >> 4).value_or(Kernel::HeaterStatuses::Unknown);

			// Byte [9] contains SolarHeater and SpaHeater bits, but is only
			// present in extended-payload packets (6+ payload bytes, 13+ total).
			// In standard 12-byte packets (5-byte payload), index 9 is the
			// checksum byte and must not be interpreted as payload.
			const bool has_extended_payload = message_bytes.size() > static_cast<uint64_t>(JandyMessage::MINIMUM_PACKET_LENGTH + STATUS_PAYLOAD_LENGTH);

			if (has_extended_payload)
			{
				m_SolarHeater = magic_enum::enum_cast<Kernel::HeaterStatuses>((message_bytes[9] & 0x70) >> 4).value_or(Kernel::HeaterStatuses::Unknown);
				m_SpaHeater = magic_enum::enum_cast<Kernel::HeaterStatuses>((message_bytes[9] & 0x07) >> 0).value_or(Kernel::HeaterStatuses::Unknown);
			}

			if (has_extended_payload)
			{
				LogDebug(
					Channel::Messages,
					std::format(
						"Status Flags -> ({} of {} bytes): (0x{:02x}) {:08B} (0x{:02x}) {:08B} (0x{:02x}) {:08B} (0x{:02x}) {:08B} (0x{:02x}) {:08B}",
						message_bytes.size(),
						message_bytes.size(),
						message_bytes[5],
						message_bytes[5],
						message_bytes[6],
						message_bytes[6],
						message_bytes[7],
						message_bytes[7],
						message_bytes[8],
						message_bytes[8],
						message_bytes[9],
						message_bytes[9]
					)
				);
			}
			else
			{
				LogDebug(
					Channel::Messages,
					std::format(
						"Status Flags -> ({} of {} bytes): (0x{:02x}) {:08B} (0x{:02x}) {:08B} (0x{:02x}) {:08B} (0x{:02x}) {:08B}",
						message_bytes.size(),
						message_bytes.size(),
						message_bytes[5],
						message_bytes[5],
						message_bytes[6],
						message_bytes[6],
						message_bytes[7],
						message_bytes[7],
						message_bytes[8],
						message_bytes[8]
					)
				);
			}

			return true;
		}

		return false;
	}

}
// namespace AqualinkAutomate::Messages
