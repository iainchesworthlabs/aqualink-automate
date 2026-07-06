#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_status.h"
#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/circulation_controller.h"
#include "devices/capabilities/command_history.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/device_actuator.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/heater_controller.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/setpoint_controller.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_unknown.h"
#include "messages/serial_adapter/serial_adapter_message_dev_ready.h"
#include "messages/serial_adapter/serial_adapter_message_dev_status.h"
#include "kernel/hub_locator.h"
#include "kernel/temperature.h"
#include "profiling/profiling.h"

namespace AqualinkAutomate::Devices
{

	class SerialAdapterDevice : public JandyController, public Capabilities::Restartable, public Capabilities::Emulated, public Capabilities::Describable, public Capabilities::DeviceActuator, public Capabilities::SetpointController, public Capabilities::CirculationController, public Capabilities::HeaterController, public Capabilities::CommandHistory
	{
		inline static constexpr std::chrono::seconds SERIALADAPTER_TIMEOUT_DURATION{ std::chrono::seconds(30) };
		inline static constexpr double SERIALADAPTER_INVALID_TEMPERATURE_CUTOFF{ -17.0 };

	public:
		struct PendingCommand
		{
			uint8_t ack_type;
			uint8_t ack_data_value;
		};

	public:
		SerialAdapterDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~SerialAdapterDevice() override;

	public:
		nlohmann::json DescribeDiagnostics() const override;

		void QueueCommand(uint8_t ack_type, uint8_t ack_data_value);
		void QueuePumpCommand(Messages::SerialAdapter_SystemPumpCommands pump, Messages::SerialAdapter_CommandTypes action);
		void QueueAuxCommand(Auxillaries::JandyAuxillaryIds aux_id, Messages::SerialAdapter_CommandTypes action);
		void QueueSetpointCommand(Messages::SerialAdapter_SystemTemperatureCommands setpoint, uint8_t temperature);

		// ----------------------------------------------------------------------
		// Capability implementations: DeviceActuator / SetpointController /
		// CirculationController. These let the capability-routed CommandDispatcher
		// drive equipment through the Serial Adapter without the dispatcher knowing
		// any Serial-Adapter-specific command codes (that mapping lives here, moved
		// out of CommandDispatcher::DispatchCommand).
		// ----------------------------------------------------------------------
		Capabilities::ActuationResult ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction action) override;
		Capabilities::ActuationResult SetPoolSetpoint(uint8_t temperature) override;
		Capabilities::ActuationResult SetSpaSetpoint(uint8_t temperature) override;
		Capabilities::ActuationResult SetCirculationMode(Kernel::CirculationModes mode) override;
		Capabilities::ActuationResult SetHeaterMode(Kernel::BodyOfWaterIds heater_body, bool enable) override;
		Capabilities::ActuationPriority ControllerPriority() const override { return Capabilities::ActuationPriority::High; }

		// CAPTURE-GATED WRITE PATH (AqualinkD-derived, NOT yet validated on this
		// project's live bus). These construct the exact RSSA control frames that
		// AqualinkD (source/serialadapter.c) emits for writes:
		//
		//   * Setpoint two-step: readySP {0x00,0x01,typeID,0x35} then
		//     setSP {0x00,0x01,0x00,val}. On the wire the device transmits the
		//     {ack_type,data} pair only (the master prepends {0x00,0x01}); the two
		//     ACKs are queued so that the readySP goes out on the next master poll
		//     and the setSP on the poll after that.
		//   * Aux toggle: setDev {0x00,0x01,state,devID} where state = RS_SA_ON
		//     (0x81) / RS_SA_OFF (0x80) and devID is the RSSA aux device id.
		//
		// These are invoked ONLY on an explicit external command -- they are never
		// auto-issued. The byte offsets/codes are validated by synthetic round-trip
		// tests and must still be confirmed against a live Brainboxes capture.
		void QueueSetpointWrite_TwoStep(Messages::SerialAdapter_SystemTemperatureCommands setpoint, uint8_t temperature);
		void QueueAuxToggleWrite(Auxillaries::JandyAuxillaryIds aux_id, bool turn_on);

		// Heater enable/disable write (POOLHT=0x11 / SPAHT=0x13 / SOLHT=0x14). Emits the RSSA
		// setDev body {0x00,0x01,state,devID} (state = SetOn 0x81 / SetOff 0x80). Validated live
		// on a real RS8-class system (2026-06-28): all three heaters confirmed actuating the
		// controller -- spa heater first, then pool heater (00 01 81 11 -> MainStatus Heating +
		// physical heater fired) and solar heater (00 01 81/80 14 -> MainStatus Enabled<->Off,
		// panel/valve responded). On the wire the device transmits the {ack_type=state,
		// data=devID} pair only; the master prepends {0x00,0x01}.
		void QueueHeaterCommand(Messages::SerialAdapter_SystemTemperatureCommands heater, bool enable);

	private:
		// Resolve the concrete SetOn/SetOff command for a Toggle actuation by inspecting
		// the device's current status trait (default SetOn; SetOff when already on/running).
		// Precondition: device carries an AuxillaryTypeTrait (checked by the caller).
		Messages::SerialAdapter_CommandTypes ResolveToggleAction(const std::shared_ptr<Kernel::AuxillaryDevice>& device) const;

		void ProcessControllerUpdates() override;

		// Emit the next queued command in response to an RSSA_DEV_READY (0x07) poll.
		// The controller solicits the second step (setSP) of a two-step setpoint write
		// with a DEV_READY poll carrying the readied type, so that value MUST be sent
		// in reply to THIS poll rather than the next CMD_STATUS (mirrors AqualinkD
		// serialadapter.c). DEV_READY is sent only as part of that handshake.
		void DrainPendingCommandForDevReady();

	private:
		void WatchdogTimeoutOccurred() override;

	private:
		void Slot_SerialAdapter_Ack(const Messages::JandyMessage_Ack& msg);
		void Slot_SerialAdapter_DevReady(const Messages::SerialAdapterMessage_DevReady& msg);
		void Slot_SerialAdapter_DevStatus(const Messages::SerialAdapterMessage_DevStatus& msg);
		void Slot_SerialAdapter_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_SerialAdapter_Status(const Messages::JandyMessage_Status& msg);
		void Slot_SerialAdapter_Unknown(const Messages::JandyMessage_Unknown& msg);

	private:
		void Command_SerialAdapter_Model(const uint16_t model_type);
		void Command_SerialAdapter_OpMode(const Messages::SerialAdapter_SCS_OpModes& op_mode);
		void Command_SerialAdapter_Options(const Messages::SerialAdapter_SCS_Options& options);
		void Command_SerialAdapter_BatteryCondition(const Messages::SerialAdapter_SCS_BatteryCondition& battery_condition);
		void Command_SerialAdapter_AirTemperature(const Kernel::Temperature& temperature);
		void Command_SerialAdapter_PoolTemperature(const Kernel::Temperature& temperature);
		void Command_SerialAdapter_SpaTemperature(const Kernel::Temperature& temperature);
		void Command_SerialAdapter_SolarTemperature(const Kernel::Temperature& temperature);
		void Command_SerialAdapter_AuxillaryStatus(const Auxillaries::JandyAuxillaryIds& aux_id, const Auxillaries::JandyAuxillaryStatuses& status);

	private:
		// Presence gating: called when a real Serial Adapter is observed answering
		// the master at this device's address. Latches emulation off permanently so
		// the emulated instance never collides with the real one on the bus.
		void DetectRealAdapterAndSuppressEmulation(std::string_view observed_message_kind);

	private:
		std::vector<Messages::SerialAdapter_StatusTypes> m_StatusTypesCollection;
		std::vector<Messages::SerialAdapter_StatusTypes>::const_iterator m_StatusTypesCollectionIter;

	private:
		bool m_StatusMessageReceived;

		// True once the emulated adapter has actively transmitted (answered a master
		// poll/probe), i.e. it has "claimed" this bus address. Used by presence-gating:
		// a DevStatus/DevReady at our id AFTER we've claimed the address is the
		// controller's solicited status push to US (the normal read path), not a real
		// adapter; only an UNSOLICITED one (before we've claimed it) implies a real
		// adapter already conversing at this address.
		bool m_HasClaimedAddress{ false };

	private:
		// FIFO of pending control ACKs. A single command queues one entry; the
		// capture-gated two-step setpoint write queues two (readySP then setSP) so
		// they are emitted on consecutive master polls in the correct order.
		std::deque<PendingCommand> m_PendingCommands{};

	private:
		Types::ProfilingUnitTypePtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Devices
