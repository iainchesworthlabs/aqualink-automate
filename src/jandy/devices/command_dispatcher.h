#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include <magic_enum/magic_enum.hpp>

#include "devices/capabilities/actuation_types.h"
#include "devices/capabilities/command_history.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/idevice.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "logging/logging.h"

namespace AqualinkAutomate::Devices
{

	class CommandDispatcher : public Interfaces::ICommandDispatcher
	{
	public:
		// Sane absolute bounds for a pool/spa water-temperature setpoint as it reaches the wire.
		//
		// The setpoint value arrives here already converted into the system's configured units
		// (Fahrenheit or Celsius) by the caller, so this guard must accommodate both: valid
		// Celsius setpoints (~4-40C) and valid Fahrenheit setpoints (~40-104F) both fall inside
		// [SETPOINT_MIN_VALUE, SETPOINT_MAX_VALUE]. The range deliberately rejects the 0 value
		// (sensor-unavailable sentinel) and anything above the maximum Fahrenheit setpoint so an
		// attacker-controlled HTTP/MQTT payload cannot push an absurd value onto the RS-485 bus.
		static constexpr uint8_t SETPOINT_MIN_VALUE{ 1 };
		static constexpr uint8_t SETPOINT_MAX_VALUE{ 104 };

		CommandDispatcher(std::shared_ptr<Kernel::DataHub> data_hub, std::shared_ptr<Kernel::EquipmentHub> equipment_hub);

		CommandResult ToggleByUuid(const boost::uuids::uuid& uuid) override;
		CommandResult ToggleByLabel(const std::string& label) override;
		CommandResult CommandByUuid(const boost::uuids::uuid& uuid, DeviceAction action) override;
		CommandResult CommandByLabel(const std::string& label, DeviceAction action) override;
		CommandResult SetPoolSetpoint(uint8_t temperature) override;
		CommandResult SetSpaSetpoint(uint8_t temperature) override;
		CommandResult SetChlorinatorPercentage(uint8_t percentage) override;
		CommandResult SetChlorinatorBoost(bool enable) override;
		CommandResult SetCirculationMode(Kernel::CirculationModes mode) override;
		CommandResult SetHeaterMode(Kernel::BodyOfWaterIds heater_body, bool enable) override;
		CommandResult SelectIAQPageButton(uint8_t button_index) override;
		CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) override;
		CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) override;
		CommandResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) override;

	private:
		// Dispatch a command to the connected controllers that advertise capability Cap,
		// trying them in ControllerPriority order (numerically-lowest = highest priority
		// first) and FALLING BACK to the next when one cannot perform the action. This
		// routes the command to whichever controller can actually act: e.g. a real
		// (non-emulated) Serial Adapter outranks an emulated OneTouch but cannot transmit,
		// so it reports NotSupported and the OneTouch then gets its turn -- rather than the
		// command being silently swallowed by the higher-priority-but-passive device.
		//
		// `action` is invoked as action(Cap&) and returns a Capabilities::ActuationResult:
		//   Accepted     -> queued for the wire; return Success (stop).
		//   InvalidValue -> the request value is bad; no controller will accept it, stop.
		//   NotSupported / MappingFailed -> this controller cannot act; try the next.
		// When every capable controller declines (or none exist) the most informative
		// failure is returned: UnknownEquipmentType if any controller reported
		// MappingFailed (capable-but-couldn't-map THIS request), else `when_none` -- which
		// preserves the historical CommandResult codes the old MapActuationResult produced.
		// `what` is a short human description of the command, used for diagnostic logging.
		template<typename Cap, typename Fn>
		CommandResult DispatchToCapable(std::string_view what, Fn&& action, CommandResult when_none) const
		{
			std::vector<std::pair<Capabilities::ActuationPriority, Cap*>> candidates;

			m_EquipmentHub->ForEachDevice([&candidates](Interfaces::IDevice& device)
			{
				if (auto* candidate = dynamic_cast<Cap*>(&device); nullptr != candidate)
				{
					auto priority = Capabilities::ActuationPriority::Lowest;
					if constexpr (requires { candidate->ControllerPriority(); })
					{
						priority = candidate->ControllerPriority();
					}
					candidates.emplace_back(priority, candidate);
				}
			});

			if (candidates.empty())
			{
				LogWarning(Logging::Channel::Devices, std::format("CommandDispatcher: No connected controller can {}", what));
				return when_none;
			}

			// Lowest ActuationPriority value wins; stable_sort keeps discovery order for ties.
			std::stable_sort(candidates.begin(), candidates.end(),
				[](const auto& lhs, const auto& rhs) { return static_cast<int>(lhs.first) < static_cast<int>(rhs.first); });

			bool any_mapping_failed{ false };

			for (auto& [priority, candidate] : candidates)
			{
				switch (action(*candidate))
				{
				case Capabilities::ActuationResult::Accepted:
					RecordOnController(candidate, what, CommandResult::Success);
					return CommandResult::Success;

				case Capabilities::ActuationResult::InvalidValue:
					RecordOnController(candidate, what, CommandResult::InvalidValue);
					return CommandResult::InvalidValue;

				case Capabilities::ActuationResult::MappingFailed:
					any_mapping_failed = true;
					break;

				case Capabilities::ActuationResult::NotSupported:
					break;
				}
			}

			LogWarning(Logging::Channel::Devices, std::format("CommandDispatcher: No capable controller could {} (tried {})", what, candidates.size()));
			return any_mapping_failed ? CommandResult::UnknownEquipmentType : when_none;
		}

		// Record a dispatched command (and its outcome) on the controller that handled it,
		// when that controller keeps a CommandHistory. The candidate is reached through a
		// polymorphic capability pointer, so a dynamic_cast cross-cast finds the history
		// mixin on the same concrete device (nullptr for controllers without one).
		template<typename Cap>
		static void RecordOnController(Cap* controller, std::string_view what, CommandResult result)
		{
			if (auto* history = dynamic_cast<Capabilities::CommandHistory*>(controller); nullptr != history)
			{
				history->RecordCommand(std::string(what), std::string(magic_enum::enum_name(result)));
			}
		}

		// Optimistic write-through of a successfully-queued chlorinator % set into the configured
		// Pool-setpoint trait that the Set-AquaPure menu scrape also populates, so reads (the
		// dashboard) reflect the new target immediately. No-op when no chlorinator is discovered.
		void WriteThroughChlorinatorSetpoint(uint8_t percentage);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub;
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
	};

}
// namespace AqualinkAutomate::Devices
