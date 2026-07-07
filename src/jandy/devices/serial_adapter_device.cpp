#include <format>
#include <functional>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "auxillaries/jandy_auxillary_traits_types.h"
#include "devices/device_status.h"
#include "logging/logging.h"
#include "devices/serial_adapter_device.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/pool_configurations.h"
#include "utility/overloaded_variant_visitor.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Messages;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

namespace AqualinkAutomate::Devices
{

	SerialAdapterDevice::SerialAdapterDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(SERIALADAPTER_TIMEOUT_DURATION),
		Capabilities::Emulated(is_emulated),
		m_ProfilingDomain(std::move(Factory::ProfilingUnitFactory::Instance().CreateDomain("SerialAdapterDevice")))
	{
		m_ProfilingDomain->Start();

		auto status_types1{ magic_enum::enum_values<Messages::SerialAdapter_SystemConfigurationStatuses>() };
		auto status_types2{ magic_enum::enum_values<Messages::SerialAdapter_SystemTemperatureCommands>() };
		auto status_types3{ magic_enum::enum_values<AqualinkAutomate::Auxillaries::JandyAuxillaryIds>() };

		m_StatusTypesCollection.insert(m_StatusTypesCollection.end(), std::make_move_iterator(status_types1.begin()), std::make_move_iterator(status_types1.end()));
		m_StatusTypesCollection.insert(m_StatusTypesCollection.end(), std::make_move_iterator(status_types2.begin()), std::make_move_iterator(status_types2.end()));
		m_StatusTypesCollection.insert(m_StatusTypesCollection.end(), std::make_move_iterator(status_types3.begin()), std::make_move_iterator(status_types3.end()));
		m_StatusTypesCollection.erase(m_StatusTypesCollection.begin());  // Don't constantly check the MODEL.

		// Move UNITS to the start of the STC section so temperature unit configuration is
		// known before any temperature/setpoint values are decoded in the first poll cycle.
		auto stc_begin = std::find_if(m_StatusTypesCollection.begin(), m_StatusTypesCollection.end(),
			[](const auto& v) { return std::holds_alternative<SerialAdapter_SystemTemperatureCommands>(v); });
		if (auto units_it = std::find_if(stc_begin, m_StatusTypesCollection.end(),
			[](const auto& v)
			{
				return std::holds_alternative<SerialAdapter_SystemTemperatureCommands>(v) &&
					std::get<SerialAdapter_SystemTemperatureCommands>(v) == SerialAdapter_SystemTemperatureCommands::UNITS;
			}); units_it != m_StatusTypesCollection.end() && units_it != stc_begin)
		{
			std::rotate(stc_begin, units_it, units_it + 1);
		}

		m_StatusTypesCollectionIter = m_StatusTypesCollection.cbegin();

		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Probe>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_Probe, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<SerialAdapterMessage_DevReady>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_DevReady, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<SerialAdapterMessage_DevStatus>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_DevStatus, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Status>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_Status, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Unknown>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_Unknown, this, std::placeholders::_1), (*device_id)());

		if (!IsEmulated())
		{
			m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Ack>(std::bind(&SerialAdapterDevice::Slot_SerialAdapter_Ack, this, std::placeholders::_1), (*device_id)());
		}

		Status(Devices::DeviceStatus_Initializing{});
	}

	SerialAdapterDevice::~SerialAdapterDevice()
	{
		m_ProfilingDomain->End();
	}

	void SerialAdapterDevice::ProcessControllerUpdates()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::ProcessControllerUpdates", std::source_location::current());

		uint8_t ack_type{ 0x00 };
		uint8_t ack_data_value{ 0x00 };

		if (m_StatusMessageReceived)
		{
			if (!m_PendingCommands.empty())
			{
				auto pending_command_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::ProcessControllerUpdates -> pending_command", std::source_location::current());

				const PendingCommand pending = m_PendingCommands.front();
				m_PendingCommands.pop_front();

				LogDebug(Channel::Devices, std::format("ProcessControllerUpdates -> Sending pending command (ack_type=0x{:02x}, ack_data_value=0x{:02x}); {} remaining", pending.ack_type, pending.ack_data_value, m_PendingCommands.size()));

				ack_type = pending.ack_type;
				ack_data_value = pending.ack_data_value;
			}
			else
			{
				auto query_status_zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::ProcessControllerUpdates -> query_status", std::source_location::current());

				LogDebug(Channel::Devices, "ProcessControllerUpdates -> Query for Status");

				std::visit(
					Utility::OverloadedVisitor
					{
						[](std::monostate)
						{
							LogWarning(Channel::Devices, "SerialAdapterDevice: Cannot select next StatusType; invalid variant content");
						},
						[&ack_type, &ack_data_value](SerialAdapter_ConfigControlCommands sa_ccc)
						{
							LogTrace(Channel::Devices, std::format("SerialAdapterDevice: Selecting Next StatusType -> {} ({:02x})", magic_enum::enum_name(sa_ccc), magic_enum::enum_integer(sa_ccc)));
							ack_type = magic_enum::enum_integer(sa_ccc);
							ack_data_value = static_cast<uint8_t>(Messages::SerialAdapter_CommandTypes::Query);
						},
						[&ack_type, &ack_data_value](SerialAdapter_SystemConfigurationStatuses sa_scs)
						{
							LogTrace(Channel::Devices, std::format("SerialAdapterDevice: Selecting Next StatusType -> {} (0x{:02x})", magic_enum::enum_name(sa_scs), magic_enum::enum_integer(sa_scs)));
							ack_type = magic_enum::enum_integer(sa_scs);
							ack_data_value = static_cast<uint8_t>(Messages::SerialAdapter_CommandTypes::Query);
						},
						[&ack_type, &ack_data_value](SerialAdapter_SystemPumpCommands sa_spc)
						{
							LogTrace(Channel::Devices, std::format("SerialAdapterDevice: Selecting Next StatusType -> {} (0x{:02x})", magic_enum::enum_name(sa_spc), magic_enum::enum_integer(sa_spc)));
							ack_type = magic_enum::enum_integer(sa_spc);
							ack_data_value = static_cast<uint8_t>(Messages::SerialAdapter_CommandTypes::Query);
						},
						[&ack_type, &ack_data_value](SerialAdapter_SystemTemperatureCommands sa_stc)
						{
							LogTrace(Channel::Devices, std::format("SerialAdapterDevice: Selecting Next StatusType -> {} (0x{:02x})", magic_enum::enum_name(sa_stc), magic_enum::enum_integer(sa_stc)));
							ack_type = magic_enum::enum_integer(sa_stc);
							ack_data_value = static_cast<uint8_t>(Messages::SerialAdapter_CommandTypes::Query);
						},
						[&ack_type, &ack_data_value](Auxillaries::JandyAuxillaryIds sa_jai)
						{
							LogTrace(Channel::Devices, std::format("SerialAdapterDevice: Selecting Next StatusType -> {} (0x{:02x})", magic_enum::enum_name(sa_jai), magic_enum::enum_integer(sa_jai) + SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET));
							ack_type = 0x00;
							ack_data_value = magic_enum::enum_integer(sa_jai) + SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET;
						},
						[]([[maybe_unused]] SerialAdapter_UnknownCommands sa_uc)
						{
							LogWarning(Channel::Devices, "SerialAdapterDevice: Cannot select next StatusType; unknown command type");
						}
					},
					*m_StatusTypesCollectionIter
				);

				if (m_StatusTypesCollection.cend() == ++m_StatusTypesCollectionIter)
				{
					m_StatusTypesCollectionIter = m_StatusTypesCollection.cbegin();

					if (Devices::DeviceStatus_Initializing{} == Status())
					{
						// Update the status to indicate that the device is now initialised.
						Status(Devices::DeviceStatus_Normal{});
					}
				}
			}

			m_StatusMessageReceived = false;
		}

		// Mark that we have actively claimed this bus address. Only an actively-emulating
		// instance actually puts a frame on the wire (Signal_AckMessage no-ops otherwise),
		// so gate on IsEmulationActive(). Presence-gating uses this to tell a solicited
		// controller status push (after we've claimed the address) from an unsolicited one.
		if (IsEmulationActive())
		{
			m_HasClaimedAddress = true;
		}

		Signal_AckMessage(ack_type, ack_data_value);
	}

	void SerialAdapterDevice::DrainPendingCommandForDevReady()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::DrainPendingCommandForDevReady", std::source_location::current());

		uint8_t ack_type{ 0x00 };
		uint8_t ack_data_value{ 0x00 };

		// A two-step setpoint write emits the readySP {typeID, 0x35} in reply to a
		// CMD_STATUS (0x02) poll; the controller then answers with an RSSA_DEV_READY
		// (0x07) poll carrying that type and expects the setSP {0x00, value} in reply
		// to THIS poll. ProcessControllerUpdates() (CMD_STATUS path) only drains the
		// queue on a fresh status message, so the setSP would otherwise miss the
		// DEV_READY window and be sent out of context on the next CMD_STATUS, where the
		// controller ignores it. Draining the queue here is safe because the controller
		// sends DEV_READY only as the second half of this handshake -- never as routine
		// traffic -- so no single-step command can be drained prematurely.
		if (!m_PendingCommands.empty())
		{
			const PendingCommand pending = m_PendingCommands.front();
			m_PendingCommands.pop_front();

			LogDebug(Channel::Devices, std::format("DrainPendingCommandForDevReady -> Sending pending command (ack_type=0x{:02x}, ack_data_value=0x{:02x}); {} remaining", pending.ack_type, pending.ack_data_value, m_PendingCommands.size()));

			ack_type = pending.ack_type;
			ack_data_value = pending.ack_data_value;
		}

		// Mark that we have actively claimed this bus address (see ProcessControllerUpdates).
		if (IsEmulationActive())
		{
			m_HasClaimedAddress = true;
		}

		Signal_AckMessage(ack_type, ack_data_value);
	}

	void SerialAdapterDevice::QueueCommand(uint8_t ack_type, uint8_t ack_data_value)
	{
		if (IsEmulated() && IsEmulationSuppressed())
		{
			// Presence gating: a real adapter owns this address. Never queue work
			// for a suppressed instance -- it must not transmit.
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice: Ignoring command (ack_type=0x{:02x}, ack_data_value=0x{:02x}) -- emulation is suppressed (a real adapter is present).", ack_type, ack_data_value));
			return;
		}

		// Single-command semantics: a freshly issued command supersedes any earlier
		// still-pending command (last-write-wins), so we clear before queuing. The
		// multi-step write path appends its follow-up step inline (see
		// QueueSetpointWrite_TwoStep) to avoid clobbering the first step.
		LogDebug(Channel::Devices, std::format("SerialAdapterDevice: Queuing command (ack_type=0x{:02x}, ack_data_value=0x{:02x})", ack_type, ack_data_value));
		m_PendingCommands.clear();
		m_PendingCommands.emplace_back(PendingCommand{ ack_type, ack_data_value });
	}

	void SerialAdapterDevice::QueuePumpCommand(Messages::SerialAdapter_SystemPumpCommands pump, Messages::SerialAdapter_CommandTypes action)
	{
		// RSSA setDev frame body is {0x00, 0x01, state, devID} (AqualinkD serialadapter.c
		// rssadapter_device_state) -- the STATE (0x81 on / 0x80 off) goes in the first data byte
		// and the device code in the second. This matches the validated QueueAuxToggleWrite order;
		// the reverse {devID, state} order is NOT recognised by the master as a set command.
		LogDebug(Channel::Devices, std::format("SerialAdapterDevice: Queuing pump command ({}, action=0x{:02x})", magic_enum::enum_name(pump), magic_enum::enum_integer(action)));
		QueueCommand(magic_enum::enum_integer(action), magic_enum::enum_integer(pump));
	}

	void SerialAdapterDevice::QueueAuxCommand(Auxillaries::JandyAuxillaryIds aux_id, Messages::SerialAdapter_CommandTypes action)
	{
		// LEGACY byte order {ack_type = devID, data = state} -- NOT used by ActuateDevice anymore
		// (the master does not recognise it as a set command). Production aux toggles go through
		// QueueAuxToggleWrite() which emits AqualinkD's {state, devID} setDev frame. Retained only
		// for the lower-level queue/sequencing unit tests; do not use for new actuation paths.
		LogDebug(Channel::Devices, std::format("SerialAdapterDevice: Queuing aux command ({}, action=0x{:02x})", magic_enum::enum_name(aux_id), magic_enum::enum_integer(action)));
		QueueCommand(magic_enum::enum_integer(aux_id) + Messages::SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET, magic_enum::enum_integer(action));
	}

	void SerialAdapterDevice::QueueSetpointCommand(Messages::SerialAdapter_SystemTemperatureCommands setpoint, uint8_t temperature)
	{
		LogDebug(Channel::Devices, std::format("SerialAdapterDevice: Queuing setpoint command ({}, temperature={})", magic_enum::enum_name(setpoint), temperature));
		QueueCommand(magic_enum::enum_integer(setpoint), temperature);
	}

	Messages::SerialAdapter_CommandTypes SerialAdapterDevice::ResolveToggleAction(const std::shared_ptr<Kernel::AuxillaryDevice>& device) const
	{
		// Auto-detect: default to SetOn; if device is already on, use SetOff.
		auto action = SerialAdapter_CommandTypes::SetOn;
		auto device_type = *(device->AuxillaryTraits[AuxillaryTypeTrait{}]);

		using enum AuxillaryTypes;
		switch (device_type)
		{
		case Auxillary:
		case Cleaner:
		case Spillover:
		case Sprinkler:
			if (auto status = device->AuxillaryTraits.TryGet(AuxillaryStatusTrait{}); status.has_value() && *status == Kernel::AuxillaryStatuses::On)
			{
				action = SerialAdapter_CommandTypes::SetOff;
			}
			break;

		case Pump:
			if (auto status = device->AuxillaryTraits.TryGet(PumpStatusTrait{}); status.has_value() && *status == Kernel::PumpStatuses::Running)
			{
				action = SerialAdapter_CommandTypes::SetOff;
			}
			break;

		default:
			break;
		}

		return action;
	}

	Capabilities::ActuationResult SerialAdapterDevice::ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction requested_action)
	{
		// Honest actuation: only an actively-emulating adapter (emulated AND not
		// presence-suppressed) can put command bytes on the wire. A real or suppressed
		// adapter would have its ACK silently dropped downstream (see emulated.h
		// Signal_AckMessage_Impl), so report NotSupported and let the dispatcher fall
		// back to another controller rather than swallowing the command.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, "SerialAdapterDevice: Not actively emulating - cannot actuate equipment");
			return Capabilities::ActuationResult::NotSupported;
		}

		// Deliverability gate: an emulated adapter can only put command bytes on the wire when
		// the master is actively polling our address -- every poll/probe Kick()s the watchdog and
		// drives ProcessControllerUpdates, which is what drains the pending-command queue. Once no
		// poll has arrived within the watchdog window IsRunning() is false (the RS Serial Adapter
		// is an optional add-on many systems never poll), so a queued command would sit forever,
		// never transmitted, while the caller is told it succeeded. Report NotSupported so the
		// CommandDispatcher falls back to a controller the master IS polling (e.g. an emulated
		// OneTouch), instead of this adapter silently swallowing the command.
		if (!IsRunning())
		{
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice ({}): master is not polling this adapter's address - cannot actuate; deferring to another controller", DeviceId()));
			return Capabilities::ActuationResult::NotSupported;
		}

		// Determine the serial command based on the requested action.
		auto action = SerialAdapter_CommandTypes::SetOn;

		if (requested_action == Capabilities::ActuationAction::Off)
		{
			action = SerialAdapter_CommandTypes::SetOff;
		}
		else if (requested_action == Capabilities::ActuationAction::Toggle && device->AuxillaryTraits.Has(AuxillaryTypeTrait{}))
		{
			action = ResolveToggleAction(device);
		}

		// Check if the device has a JandyAuxillaryId trait (i.e. a hardware aux port).
		if (device->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}))
		{
			auto aux_id = *(device->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]);
			const bool turn_on{ action == SerialAdapter_CommandTypes::SetOn };
			LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Actuating aux command for {} ({})", magic_enum::enum_name(aux_id), turn_on ? "ON" : "OFF"));
			// Emit AqualinkD's validated setDev frame {state, devID} via QueueAuxToggleWrite.
			// The older QueueAuxCommand emitted the bytes in the REVERSE order {devID, state},
			// which the master does not recognise as an aux set command (its state byte 0x80/0x81
			// is the recognised command code, and the device id belongs in the data byte - mirroring
			// the aux STATUS query, which already sends {ack_type=0x00, data=devID}).
			QueueAuxToggleWrite(aux_id, turn_on);
			return Capabilities::ActuationResult::Accepted;
		}

		// No aux ID - try to map as a system pump command via type and label.
		if (device->AuxillaryTraits.Has(AuxillaryTypeTrait{}))
		{
			auto device_type = *(device->AuxillaryTraits[AuxillaryTypeTrait{}]);
			auto label_opt = device->AuxillaryTraits.TryGet(LabelTrait{});
			std::string label = label_opt.value_or("");

			switch (device_type)
			{
			case AuxillaryTypes::Pump:
				if (label.contains("Filter") || label.contains("Pump"))
				{
					LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Actuating pump command PUMP (action=0x{:02x})", magic_enum::enum_integer(action)));
					QueuePumpCommand(SerialAdapter_SystemPumpCommands::PUMP, action);
					return Capabilities::ActuationResult::Accepted;
				}
				break;

			case AuxillaryTypes::Cleaner:
				LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Actuating pump command CLEANR (action=0x{:02x})", magic_enum::enum_integer(action)));
				QueuePumpCommand(SerialAdapter_SystemPumpCommands::CLEANR, action);
				return Capabilities::ActuationResult::Accepted;

			case AuxillaryTypes::Spillover:
				LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Actuating pump command SPILLOVER (action=0x{:02x})", magic_enum::enum_integer(action)));
				QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPILLOVER, action);
				return Capabilities::ActuationResult::Accepted;

			default:
				break;
			}

			// Check if it's a spa device by label.
			if (label.contains("Spa"))
			{
				LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Actuating pump command SPA (action=0x{:02x})", magic_enum::enum_integer(action)));
				QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPA, action);
				return Capabilities::ActuationResult::Accepted;
			}
		}

		LogWarning(Channel::Devices, "SerialAdapterDevice: Could not map device to a serial adapter command");
		return Capabilities::ActuationResult::MappingFailed;
	}

	Capabilities::ActuationResult SerialAdapterDevice::SetPoolSetpoint(uint8_t temperature)
	{
		// Only an actively-emulating adapter can transmit; otherwise the queued command
		// is silently dropped. Report NotSupported so the dispatcher can fall back.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, "SerialAdapterDevice: Not actively emulating - cannot set pool setpoint");
			return Capabilities::ActuationResult::NotSupported;
		}

		LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Setting pool setpoint to {}", temperature));
		// AqualinkD drives setpoints as a two-step readySP/setSP sequence (serialadapter.c
		// queue_aqualink_rssadapter_setpoint), not a single frame. The single-frame form was not
		// recognised by the controller (same class as the command byte-order bug).
		QueueSetpointWrite_TwoStep(SerialAdapter_SystemTemperatureCommands::POOLSP, temperature);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult SerialAdapterDevice::SetSpaSetpoint(uint8_t temperature)
	{
		// Only an actively-emulating adapter can transmit; otherwise the queued command
		// is silently dropped. Report NotSupported so the dispatcher can fall back.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, "SerialAdapterDevice: Not actively emulating - cannot set spa setpoint");
			return Capabilities::ActuationResult::NotSupported;
		}

		LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Setting spa setpoint to {}", temperature));
		// Two-step readySP/setSP sequence (see SetPoolSetpoint).
		QueueSetpointWrite_TwoStep(SerialAdapter_SystemTemperatureCommands::SPASP, temperature);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult SerialAdapterDevice::SetCirculationMode(Kernel::CirculationModes mode)
	{
		// Only an actively-emulating adapter can transmit; otherwise the queued command
		// is silently dropped. Report NotSupported so the dispatcher can fall back.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, "SerialAdapterDevice: Not actively emulating - cannot set circulation mode");
			return Capabilities::ActuationResult::NotSupported;
		}

		// SPA and SPILLOVER move water between two bodies, so they only exist on a system that HAS
		// two bodies (pool + spa). On a known single-body system the RS Serial Adapter's SPA/
		// SPILLOVER commands are meaningless (AquaLink RS Serial Adapter host protocol, note 4:
		// SPA applies to combo / dual-equipment models only -- not pool-only or spa-only). Reject
		// them when the configuration is KNOWN to be single-body; stay permissive while it is still
		// Unknown (we then lean on the equipment actually discovered, as elsewhere).
		const bool known_single_body = (nullptr != m_DataHub) &&
			(Kernel::PoolConfigurations::SingleBody == m_DataHub->PoolConfiguration);

		switch (mode)
		{
		case Kernel::CirculationModes::Spa:
			if (known_single_body)
			{
				LogWarning(Channel::Devices, "SerialAdapterDevice: Spa circulation is not available on a single-body (pool-only/spa-only) system");
				return Capabilities::ActuationResult::NotSupported;
			}
			LogInfo(Channel::Devices, "SerialAdapterDevice: Setting circulation mode to Spa");
			QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPA, SerialAdapter_CommandTypes::SetOn);
			return Capabilities::ActuationResult::Accepted;

		case Kernel::CirculationModes::Pool:
			LogInfo(Channel::Devices, "SerialAdapterDevice: Setting circulation mode to Pool");
			QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPA, SerialAdapter_CommandTypes::SetOff);
			return Capabilities::ActuationResult::Accepted;

		case Kernel::CirculationModes::Spillover:
			if (known_single_body)
			{
				LogWarning(Channel::Devices, "SerialAdapterDevice: Spillover is not available on a single-body (pool-only/spa-only) system");
				return Capabilities::ActuationResult::NotSupported;
			}
			LogInfo(Channel::Devices, "SerialAdapterDevice: Setting circulation mode to Spillover");
			QueuePumpCommand(SerialAdapter_SystemPumpCommands::SPILLOVER, SerialAdapter_CommandTypes::SetOn);
			return Capabilities::ActuationResult::Accepted;

		case Kernel::CirculationModes::SpaFill:
		case Kernel::CirculationModes::SpaDrain:
			// Spa Fill / Spa Drain are valve-sequencing / service operations: the Jandy RS
			// protocol has no single command for them (the RS Serial Adapter exposes only the
			// SPA and SPILLOVER pump commands), so they cannot be driven remotely. Report
			// NotSupported (a well-formed request this controller cannot perform) rather than
			// the generic InvalidValue. See OBS-06 in docs/alwin32/sim-validation-findings.md.
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice: Circulation mode {} is not remotely commandable - no RS-485 command exists for it", magic_enum::enum_name(mode)));
			return Capabilities::ActuationResult::NotSupported;

		default:
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice: Invalid circulation mode: {}", magic_enum::enum_name(mode)));
			return Capabilities::ActuationResult::InvalidValue;
		}
	}

	Capabilities::ActuationResult SerialAdapterDevice::SetHeaterMode(Kernel::BodyOfWaterIds heater_body, bool enable)
	{
		// Only an actively-emulating adapter can transmit; otherwise the queued command is silently
		// dropped. Report NotSupported so the dispatcher can fall back.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, "SerialAdapterDevice: Not actively emulating - cannot set heater mode");
			return Capabilities::ActuationResult::NotSupported;
		}

		// Map the heater's body of water to its RSSA heater command. Shared == the solar heater.
		SerialAdapter_SystemTemperatureCommands heater_cmd{};
		switch (heater_body)
		{
		case Kernel::BodyOfWaterIds::Pool:
			heater_cmd = SerialAdapter_SystemTemperatureCommands::POOLHT;
			break;
		case Kernel::BodyOfWaterIds::Spa:
			heater_cmd = SerialAdapter_SystemTemperatureCommands::SPAHT;
			break;
		case Kernel::BodyOfWaterIds::Shared:
			heater_cmd = SerialAdapter_SystemTemperatureCommands::SOLHT;
			break;
		default:
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice: No heater command for body {}", magic_enum::enum_name(heater_body)));
			return Capabilities::ActuationResult::MappingFailed;
		}

		LogInfo(Channel::Devices, std::format("SerialAdapterDevice: Setting {} heater {}", magic_enum::enum_name(heater_body), enable ? "ON" : "OFF"));
		QueueHeaterCommand(heater_cmd, enable);
		return Capabilities::ActuationResult::Accepted;
	}

	void SerialAdapterDevice::QueueSetpointWrite_TwoStep(Messages::SerialAdapter_SystemTemperatureCommands setpoint, uint8_t temperature)
	{
		// TWO-STEP SETPOINT WRITE (AqualinkD source/serialadapter.c). Live-validated on a
		// dual-body iAQ/OneTouch + AquaRite controller, 2026-06-28 (pool 30->29C and spa
		// 38->37C both actuated on the wire and the physical panel).
		//
		//   Step 1 (readySP): wire {0x00,0x01,typeID,0x35}, in reply to a CMD_STATUS poll
		//                     -> ACK {ack_type = typeID(setpoint), data = 0x35}
		//   Step 2 (setSP):   wire {0x00,0x01,0x00,val}
		//                     -> ACK {ack_type = 0x00,            data = temperature}
		//
		// The two ACKs are queued FIFO. CRUCIAL TIMING: after the readySP the controller
		// answers with an RSSA_DEV_READY (0x07) poll carrying the readied type, and the
		// setSP value MUST be sent in reply to THAT poll -- Slot_SerialAdapter_DevReady ->
		// DrainPendingCommandForDevReady() handles this. Sending the setSP on a later
		// CMD_STATUS poll instead (the original behaviour) is ignored by the controller.
		LogNotify(Channel::Devices, std::format("SerialAdapterDevice: Queuing two-step setpoint write ({}, temperature={}) -- readySP then setSP", magic_enum::enum_name(setpoint), temperature));

		// Supersede any earlier pending command, then append both steps in order so the
		// readySP drains on the CMD_STATUS poll and the setSP on the DEV_READY poll. The
		// second step is APPENDED inline (a second QueueCommand must NOT be used -- it would
		// clear the first step).
		QueueCommand(magic_enum::enum_integer(setpoint), magic_enum::enum_integer(Messages::SerialAdapter_CommandTypes::ReadySetpoint));

		if (IsEmulated() && IsEmulationSuppressed())
		{
			// Presence gating: a real adapter owns this address; stay passive.
			LogWarning(Channel::Devices, std::format("SerialAdapterDevice: Ignoring enqueued command (ack_type=0x{:02x}, ack_data_value=0x{:02x}) -- emulation is suppressed (a real adapter is present).", 0x00, temperature));
			return;
		}

		// Append (FIFO) the setSP step without discarding the readySP queued just above.
		LogDebug(Channel::Devices, std::format("SerialAdapterDevice: Enqueuing command (ack_type=0x{:02x}, ack_data_value=0x{:02x})", 0x00, temperature));
		m_PendingCommands.emplace_back(PendingCommand{ 0x00, temperature });
	}

	void SerialAdapterDevice::QueueAuxToggleWrite(Auxillaries::JandyAuxillaryIds aux_id, bool turn_on)
	{
		// AUX ON/OFF WRITE (AqualinkD source/serialadapter.c, setDev). This is the aux-toggle
		// frame ActuateDevice() emits for a hardware aux.
		//   setDev[] = {0x00, 0x01, state, devID}, state = RS_SA_ON(0x81)/OFF(0x80).
		//   -> ACK {ack_type = state, data = devID(aux + SERIALADAPTER_AUX_ID_OFFSET)}
		//
		// NOTE the byte ORDER is the REVERSE of the legacy QueueAuxCommand() (which sent
		// {ack_type = devID, data = state}); AqualinkD puts the state in the command byte and
		// the device id in the value byte, matching the aux STATUS query ({ack_type = 0x00,
		// data = devID}). The legacy order is not recognised by the master as a set command.
		// This {state, devID} order is AqualinkD-derived; it is being confirmed on a live bus
		// (2026-06-17) -- if that test refutes it, revert ActuateDevice to the other order.
		const uint8_t state{ static_cast<uint8_t>(turn_on ? Messages::SerialAdapter_CommandTypes::SetOn : Messages::SerialAdapter_CommandTypes::SetOff) };
		const uint8_t dev_id{ static_cast<uint8_t>(magic_enum::enum_integer(aux_id) + Messages::SerialAdapterMessage_DevStatus::SERIALADAPTER_AUX_ID_OFFSET) };

		LogNotify(Channel::Devices, std::format("SerialAdapterDevice: [CAPTURE-GATED] Queuing aux toggle write ({}, {}) -> setDev state=0x{:02x} devID=0x{:02x}", magic_enum::enum_name(aux_id), turn_on ? "ON" : "OFF", state, dev_id));

		QueueCommand(state, dev_id);
	}

	void SerialAdapterDevice::QueueHeaterCommand(Messages::SerialAdapter_SystemTemperatureCommands heater, bool enable)
	{
		// HEATER ON/OFF WRITE (POOLHT=0x11 / SPAHT=0x13 / SOLHT=0x14). The heater is a device in
		// the RSSA setDev family, so the frame body is {0x00, 0x01, state, devID} -- STATE
		// (SetOn 0x81 / SetOff 0x80) first, heater device code second -- exactly like the validated
		// aux/pump toggle order (AqualinkD serialadapter.c rssadapter_device_state). The earlier
		// {devID, state} order was not recognised by the master.
		const uint8_t state{ static_cast<uint8_t>(enable ? Messages::SerialAdapter_CommandTypes::SetOn : Messages::SerialAdapter_CommandTypes::SetOff) };

		LogNotify(Channel::Devices, std::format("SerialAdapterDevice: Queuing heater write ({}, {}) -> state=0x{:02x} devID=0x{:02x}", magic_enum::enum_name(heater), enable ? "ON" : "OFF", state, magic_enum::enum_integer(heater)));

		QueueCommand(state, magic_enum::enum_integer(heater));
	}

	void SerialAdapterDevice::WatchdogTimeoutOccurred()
	{
		// Intentionally empty: the Serial Adapter is an emulated controller that owns the bus
		// conversation, so a watchdog timeout requires no device-side state change here.
	}

	nlohmann::json SerialAdapterDevice::DescribeDiagnostics() const
	{
		nlohmann::json j;

		j["device_type"] = "SerialAdapter";
		j["device_id"] = std::format("0x{:02x}", DeviceId().Id()());
		j["status_collection_count"] = static_cast<uint32_t>(m_StatusTypesCollection.size());
		j["status_message_received"] = m_StatusMessageReceived;
		j["has_pending_command"] = !m_PendingCommands.empty();
		j["pending_command_count"] = static_cast<uint32_t>(m_PendingCommands.size());
		j["is_emulated"] = IsEmulated();
		j["emulation_suppressed"] = IsEmulationSuppressed();
		j["recent_commands"] = DescribeRecentCommands();
		j["is_running"] = IsRunning();

		return j;
	}

}
// namespace AqualinkAutomate::Devices
