#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <span>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/temperature.h"
#include "messages/iaq/iaq_message.h"

namespace AqualinkAutomate::Messages
{

	class IAQMessage_MainStatus : public IAQMessage, public Interfaces::IMessageSignalRecv<IAQMessage_MainStatus>
	{
	public:
		IAQMessage_MainStatus() noexcept;
		~IAQMessage_MainStatus() override = default;

	public:
		const std::vector<uint8_t>& RawPayload() const;
		bool PumpOn() const;
		bool SpaMode() const;

		// Actual (measured) temperatures. The current wire format only carries the
		// ACTIVE body's water temperature (and only while the pump is running), so
		// pool/spa are optional: absent means "no reading in this message", never 0.
		std::optional<Kernel::Temperature> PoolTemperature() const;
		std::optional<Kernel::Temperature> SpaTemperature() const;
		std::optional<Kernel::Temperature> AirTemperature() const;

		// Heat setpoints (targets). The current format reports both on every message;
		// the legacy format reports only the active body's target (see HeaterSetpoint).
		std::optional<Kernel::Temperature> PoolSetpoint() const;
		std::optional<Kernel::Temperature> SpaSetpoint() const;

		// The active body's target: spa target in spa mode, else pool target.
		std::optional<Kernel::Temperature> HeaterSetpoint() const;
		Kernel::HeaterStatuses PoolHeaterStatus() const;
		Kernel::HeaterStatuses SpaHeaterStatus() const;
		Kernel::HeaterStatuses SolarHeaterStatus() const;
		const std::vector<uint8_t>& DeviceIds() const;

	public:
		std::string ToString() const override;

	public:
		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		std::vector<uint8_t> m_RawPayload;
		bool m_PumpOn{false};
		bool m_SpaMode{false};
		std::optional<Kernel::Temperature> m_PoolTemp;
		std::optional<Kernel::Temperature> m_SpaTemp;
		std::optional<Kernel::Temperature> m_AirTemp;
		std::optional<Kernel::Temperature> m_PoolSetpoint;
		std::optional<Kernel::Temperature> m_SpaSetpoint;
		std::optional<Kernel::Temperature> m_HeaterSetpoint;
		Kernel::HeaterStatuses m_PoolHeaterStatus{Kernel::HeaterStatuses::Unknown};
		Kernel::HeaterStatuses m_SpaHeaterStatus{Kernel::HeaterStatuses::Unknown};
		Kernel::HeaterStatuses m_SolarHeaterStatus{Kernel::HeaterStatuses::Unknown};
		std::vector<uint8_t> m_DeviceIds;
	};

}
// namespace AqualinkAutomate::Messages
