#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <boost/uuid/uuid.hpp>

#include "devices/capabilities/restartable.h"
#include "devices/pentair_device.h"
#include "devices/pentair_device_id.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "messages/pump/pentair_pump_message_status.h"

namespace AqualinkAutomate::Pentair::Devices
{

	// Handler for a Pentair variable-speed pump (IntelliFlo, addresses 0x60-0x6F).
	//
	// Decodes the pump's status broadcasts (RPM / watts / GPM), keeps a watchdog
	// alive while the pump is reporting, surfaces the pump as a Pump auxillary in
	// the DataHub, and can emit Speed (0x01) and Power (0x06) commands via the
	// message send-signals.
	//
	// Messages are matched on their FROM address inside the status slot (status
	// frames are broadcast pump -> controller, so FROM is the pump address).
	class PentairVSPPumpDevice : public PentairDevice, public AqualinkAutomate::Devices::Capabilities::Restartable
	{
		inline static const std::chrono::seconds PUMP_TIMEOUT_DURATION{ std::chrono::seconds(30) };

	public:
		// A single timestamped-reading alias parameterised on the value type, in
		// place of three near-identical RPM/Watts/GPM pair aliases.
		template<typename VALUE>
		using Timestamped = std::pair<VALUE, std::chrono::time_point<std::chrono::system_clock>>;

		using TimestampedRPM = Timestamped<uint16_t>;
		using TimestampedWatts = Timestamped<uint16_t>;
		using TimestampedGPM = Timestamped<uint8_t>;

	public:
		PentairVSPPumpDevice(const std::shared_ptr<PentairDeviceId>& device_id, Kernel::HubLocator& hub_locator);
		~PentairVSPPumpDevice() override = default;

		TimestampedRPM ReportedRPM() const;
		TimestampedWatts ReportedWatts() const;
		TimestampedGPM ReportedGPM() const;
		// Named for the motor, not the watchdog: the Restartable capability this device
		// also mixes in has its own IsRunning() meaning "the watchdog is live". Two
		// unrelated questions, so they do not share a name.
		bool IsPumpRunning() const;

		// Emit a set-speed command (controller -> this pump).
		void SetSpeed(uint16_t rpm) const;

		// Emit a power on/off command (controller -> this pump).
		void SetPower(bool power_on) const;

	protected:
		// Resets the live operating point and the DataHub pump status/flow when the
		// pump stops reporting.  Protected (matching the Restartable base contract)
		// so a focused test can drive the timeout deterministically.
		void WatchdogTimeoutOccurred() override;

	private:
		void Slot_Pump_Status(const Messages::PentairPumpMessage_Status& msg);

		// Returns the cached DataHub pump auxillary for this device, creating it on
		// first use.  The handle is cached by uuid so subsequent status frames do a
		// single O(1) FindById() instead of scanning Pumps() and comparing a freshly
		// formatted label string each time.
		std::shared_ptr<Kernel::AuxillaryDevice> ResolvePumpDevice();
		void PushStatusToDataHub(const Messages::PentairPumpMessage_Status& msg);

	private:
		uint8_t m_Address;
		TimestampedRPM m_RPM{ std::make_pair(0, std::chrono::system_clock::now()) };
		TimestampedWatts m_Watts{ std::make_pair(0, std::chrono::system_clock::now()) };
		TimestampedGPM m_GPM{ std::make_pair(0, std::chrono::system_clock::now()) };
		bool m_IsRunning{ false };
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };

		// Identity of the DataHub pump auxillary this device owns; resolved once.
		std::optional<boost::uuids::uuid> m_PumpDeviceId;
	};

}
// namespace AqualinkAutomate::Pentair::Devices
