#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/auxillary_devices/pump_status.h"
//#include "factories/jandy_message_factory_registration.h"
#include "messages/jandy_message.h"

namespace AqualinkAutomate::Messages
{

	enum class ComboModes : uint8_t
	{
		Pool = 0x00,
		Spa = 0x01,
		Unknown = 0x02
	};

	class JandyMessage_Status : public JandyMessage, public Interfaces::IMessageSignalRecv<JandyMessage_Status>
	{
		static const uint8_t STATUS_PAYLOAD_LENGTH;

	public:
		JandyMessage_Status() noexcept;
		~JandyMessage_Status() override = default;

	public:
		ComboModes Mode() const;
		Kernel::PumpStatuses FilterPump() const;
		Kernel::AuxillaryStatuses Aux1() const;
		Kernel::AuxillaryStatuses Aux2() const;
		Kernel::AuxillaryStatuses Aux3() const;
		Kernel::AuxillaryStatuses Aux4() const;
		Kernel::AuxillaryStatuses Aux5() const;
		Kernel::AuxillaryStatuses Aux6() const;
		Kernel::AuxillaryStatuses Aux7() const;
		Kernel::HeaterStatuses PoolHeater() const;
		Kernel::HeaterStatuses SpaHeater() const;
		Kernel::HeaterStatuses SolarHeater() const;

	public:
		// Raw payload bytes retained verbatim from the wire (between the 4-byte header and
		// 3-byte footer). Empty until DeserializeContents runs. The structured Aux*/Heater
		// accessors above decode payload bytes [1..] (wire indices 5..9); payload byte [0]
		// (wire index 4) is NOT covered by them. This accessor exposes the full payload so a
		// handler that needs that first byte -- e.g. a spa-side remote decoding the master's
		// cmd-0x02 LED-indicator image, whose indicator byte is payload[0] -- can read it.
		const std::vector<uint8_t>& RawPayload() const;

		std::string ToString() const override;

	public:
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::vector<uint8_t> m_RawPayload;
		ComboModes m_Mode;
		Kernel::PumpStatuses m_FilterPump;
		Kernel::AuxillaryStatuses m_Aux1;
		Kernel::AuxillaryStatuses m_Aux2;
		Kernel::AuxillaryStatuses m_Aux3;
		Kernel::AuxillaryStatuses m_Aux4;
		Kernel::AuxillaryStatuses m_Aux5;
		Kernel::AuxillaryStatuses m_Aux6;
		Kernel::AuxillaryStatuses m_Aux7;
		Kernel::HeaterStatuses m_PoolHeater;
		Kernel::HeaterStatuses m_SolarHeater;
		Kernel::HeaterStatuses m_SpaHeater;
	};

}
// namespace AqualinkAutomate::Messages
