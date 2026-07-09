#include <algorithm>
#include <format>
#include <source_location>
#include <string_view>
#include <variant>

#include "logging/logging.h"
#include "devices/serial_adapter_device.h"
#include "formatters/jandy_device_formatters.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	void SerialAdapterDevice::Slot_SerialAdapter_Ack([[maybe_unused]] const Messages::JandyMessage_Ack& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_Ack", std::source_location::current());
		LogDebug(Channel::Devices, "Serial Adapter device received a JandyMessage_Ack signal.");

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::Slot_SerialAdapter_DevReady([[maybe_unused]] const Messages::SerialAdapterMessage_DevReady& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_DevReady", std::source_location::current());
		LogDebug(Channel::Devices, "Serial Adapter device received a SerialAdapterMessage_DevReady signal.");

		// A DevReady (0x07) at this address is a response FROM a real adapter TO the
		// master -- the emulated instance never sends one itself. Seeing it means a
		// real Serial Adapter is present; go passive so we never double-transmit.
		DetectRealAdapterAndSuppressEmulation("DevReady");

		// A DEV_READY poll is the controller soliciting the second step (setSP) of a
		// two-step setpoint write, so drain a queued command here rather than via the
		// CMD_STATUS-gated ProcessControllerUpdates() (which would defer the value to the
		// next CMD_STATUS poll, out of the handshake context, and the controller ignores it).
		DrainPendingCommandForDevReady();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::Slot_SerialAdapter_DevStatus(const Messages::SerialAdapterMessage_DevStatus& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_DevStatus", std::source_location::current());
		LogDebug(Channel::Devices, "Serial Adapter device received a SerialAdapterMessage_DevStatus signal.");

		// A DevStatus (0x13) at this address is a real adapter's reply to the master
		// (the emulated device replies to polls with ACKs, never with a DevStatus).
		// Detecting it means a real adapter is present; latch emulation off so we
		// only ever passively decode its replies from here on.
		DetectRealAdapterAndSuppressEmulation("DevStatus");

		if (msg.Options().has_value() && msg.Options().value().HasCleaner)
		{
			auto it = std::find_if(m_StatusTypesCollection.begin(), m_StatusTypesCollection.end(), 
				[](const auto& v)
				{
					return 
						std::holds_alternative<Auxillaries::JandyAuxillaryIds>(v) &&
						std::get<Auxillaries::JandyAuxillaryIds>(v) == Auxillaries::JandyAuxillaryIds::Aux_1;
				}
			);

			if (m_StatusTypesCollection.end() != it)
			{
				// AUX1 is configured as the CLEANER -> checking AUX1 ON/OFF will error so check the "CLEANR" instead.
				*it = Messages::SerialAdapter_SystemPumpCommands::CLEANR;
			}
		}
		
		if (msg.Options().has_value() && msg.Options().value().HasSpillover)
		{
			auto it = std::find_if(m_StatusTypesCollection.begin(), m_StatusTypesCollection.end(),
				[](const auto& v)
				{
					return
						std::holds_alternative<Auxillaries::JandyAuxillaryIds>(v) &&
						std::get<Auxillaries::JandyAuxillaryIds>(v) == Auxillaries::JandyAuxillaryIds::Aux_3;
				}
			);

			if (m_StatusTypesCollection.end() != it)
			{
				// AUX3 is configured as the SPA SPILLOVER -> checking AUX3 ON/OFF will error so check the "SPILLOVER" instead.
				*it = Messages::SerialAdapter_SystemPumpCommands::SPILLOVER;
			}
		}

		if (msg.Options().has_value() && !m_StatusTypesCollection.empty())
		{
			// Stop polling OPTIONS now that it has been read, but ONLY if the front element
			// is still the OPTIONS entry. Without this guard a second OPTIONS-bearing message
			// (or any later one) would blindly erase whatever is now at the front (e.g. OPMODE),
			// corrupting the poll rotation.
			const auto& front = m_StatusTypesCollection.front();
			const bool front_is_options =
				std::holds_alternative<Messages::SerialAdapter_SystemConfigurationStatuses>(front) &&
				std::get<Messages::SerialAdapter_SystemConfigurationStatuses>(front) == Messages::SerialAdapter_SystemConfigurationStatuses::OPTIONS;

			if (front_is_options)
			{
				m_StatusTypesCollection.erase(m_StatusTypesCollection.begin()); // Don't constantly check the OPTIONS.
				m_StatusTypesCollectionIter = m_StatusTypesCollection.cbegin(); // Iterators were invalidated; reset to OPMODE.
			}
		}

		//
		// Process any updates to the monitored equipment.
		//

		if (msg.ModelType().has_value())
		{
			Command_SerialAdapter_Model(msg.ModelType().value());
		}

		if (msg.OpMode().has_value())
		{
			Command_SerialAdapter_OpMode(msg.OpMode().value());
		}

		if (msg.Options().has_value())
		{
			Command_SerialAdapter_Options(msg.Options().value());
		}

		if (msg.BatteryCondition().has_value())
		{
			Command_SerialAdapter_BatteryCondition(msg.BatteryCondition().value());
		}

		if (msg.TemperatureUnits().has_value())
		{
			JandyController::m_DataHub->SystemTemperatureUnits(msg.TemperatureUnits().value());
		}

		// Convert raw temperature bytes using the system's configured temperature units.
		// The RSSA sends temperatures as raw integer values in the system's unit system
		// (Fahrenheit or Celsius), determined by the UNITS (0x0A) query response.
		auto convert_raw_temperature = [this](uint8_t raw_value) -> Kernel::Temperature
		{
			if (JandyController::m_DataHub->SystemTemperatureUnits() == Kernel::TemperatureUnits::Celsius)
			{
				return Kernel::Temperature::ConvertToTemperatureInCelsius(static_cast<double>(raw_value));
			}
			else
			{
				return Kernel::Temperature::ConvertToTemperatureInFahrenheit(static_cast<double>(raw_value));
			}
		};

		if (msg.Pool_SetPoint_One().has_value())
		{
			JandyController::m_DataHub->PoolTempSetpoint(convert_raw_temperature(msg.Pool_SetPoint_One().value()));
		}

		if (msg.Pool_SetPoint_Two().has_value())
		{
			// POOLSP2 == the panel's "TEMP2" maintenance (low) setpoint, reported only on single-body
			// (Pool-Only/Spa-Only) systems alongside the priority TEMP1 (POOLSP).
			JandyController::m_DataHub->PoolTempSetpoint2(convert_raw_temperature(msg.Pool_SetPoint_Two().value()));
		}

		if (msg.Pool_Heater_Two_Enabled().has_value())
		{
			// POOLHT2 == whether the "TEMP2" maintenance heating is enabled (capture-gated decode).
			JandyController::m_DataHub->PoolHeater2Enabled(msg.Pool_Heater_Two_Enabled().value());
		}

		if (msg.Spa_SetPoint().has_value())
		{
			JandyController::m_DataHub->SpaTempSetpoint(convert_raw_temperature(msg.Spa_SetPoint().value()));
		}

		// A raw temperature value of 0 means "sensor not available" (e.g. pump off,
		// no water flowing through sensors). Skip writing to avoid overwriting valid
		// temperatures reported by other emulated devices.

		if (msg.AirTemperature().has_value() && msg.AirTemperature().value() != 0)
		{
			Command_SerialAdapter_AirTemperature(convert_raw_temperature(msg.AirTemperature().value()));
		}

		if (msg.PoolTemperature().has_value() && msg.PoolTemperature().value() != 0)
		{
			Command_SerialAdapter_PoolTemperature(convert_raw_temperature(msg.PoolTemperature().value()));
		}

		if (msg.SpaTemperature().has_value() && msg.SpaTemperature().value() != 0)
		{
			Command_SerialAdapter_SpaTemperature(convert_raw_temperature(msg.SpaTemperature().value()));
		}

		if (msg.SolarTemperature().has_value() && msg.SolarTemperature().value() != 0)
		{
			Command_SerialAdapter_SolarTemperature(convert_raw_temperature(msg.SolarTemperature().value()));
		}

		if (msg.AuxilliaryState().has_value())
		{
			auto aux_id = std::get<Auxillaries::JandyAuxillaryIds>(msg.AuxilliaryState().value());
			auto jas_state = std::get<std::optional<Auxillaries::JandyAuxillaryStatuses>>(msg.AuxilliaryState().value());
			
			if (jas_state.has_value())
			{
				Command_SerialAdapter_AuxillaryStatus(aux_id, jas_state.value());
			}
		}

		//
		// Finalise message and state processing.
		//
		
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::Slot_SerialAdapter_Probe([[maybe_unused]] const Messages::JandyMessage_Probe& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_Probe", std::source_location::current());
		LogDebug(Channel::Devices, "Serial Adapter device received a JandyMessage_Probe signal.");

		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::Slot_SerialAdapter_Status([[maybe_unused]] const Messages::JandyMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_Status", std::source_location::current());
		LogDebug(Channel::Devices, "Serial Adapter device received a JandyMessage_Status signal.");

		m_StatusMessageReceived = true;

		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::Slot_SerialAdapter_Unknown(const Messages::JandyMessage_Unknown& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SerialAdapterDevice::Slot_Unknown", std::source_location::current());
		LogDebug(Channel::Devices, [&msg]() { return std::format("Serial Adapter device received a JandyMessage_Unknown signal: type -> 0x{:02x}", msg.RawId()); });

		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void SerialAdapterDevice::DetectRealAdapterAndSuppressEmulation(std::string_view observed_message_kind)
	{
		// A non-emulated instance is already passive, so there is nothing to do.
		if (!IsEmulated() || IsEmulationSuppressed())
		{
			return;
		}

		// Opt-out (--jandy-disable-presence-gating): never auto-suppress.
		if ((nullptr != JandyController::m_DataHub) && JandyController::m_DataHub->PresenceGatingDisabled)
		{
			return;
		}

		// Presence gating, corrected against live captures:
		//
		// A DevStatus(0x13)/DevReady(0x07) frame addressed to OUR id is the CONTROLLER's
		// status push to whichever adapter currently owns this address -- it is the normal
		// RSSA read path, NOT a real adapter's output. (A real adapter answers the master
		// with an Ack to 0x00; it never emits these inbound frames. Captured live: with no
		// emulation present, zero such frames appear on the bus; they begin only AFTER an
		// adapter answers the master's poll and claims the address.)
		//
		// So once WE have claimed the address (transmitted at least once as the active
		// emulation), such a frame is SOLICITED -- the controller talking to us -- and must
		// NOT suppress. Treating it as a collision was a false positive that silenced our
		// own emulation on the very first status push. We suppress ONLY on an UNSOLICITED
		// frame: one seen before we ever claimed the address, which means another (real)
		// adapter is already conversing here at start-up.
		if (m_HasClaimedAddress)
		{
			LogTrace(Channel::Devices, std::format("Serial Adapter ({}): solicited {} frame (controller status push to our emulation) -- not a real-adapter collision.", DeviceId(), observed_message_kind));
			return;
		}

		LogNotify(Channel::Devices, std::format("Serial Adapter ({}): an UNSOLICITED {} frame was observed before we claimed this address -- a REAL adapter is present; handling the bus collision (relocate to a free instance if possible, else suppress).", DeviceId(), observed_message_kind));

		// Prefer relocation to a free SerialAdapter instance (0x48/0x49 both exist) over going
		// silent; falls back to suppression when no relocation handler / no free instance.
		HandleEmulationCollision();

		// Drop anything we might have queued -- this instance must not emit at this address.
		m_PendingCommands.clear();
	}

}
// namespace AqualinkAutomate::Devices
