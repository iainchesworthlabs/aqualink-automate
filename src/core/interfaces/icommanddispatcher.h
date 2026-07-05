#pragma once

#include <cstdint>
#include <string>

#include <boost/uuid/uuid.hpp>

#include "kernel/body_of_water_ids.h"
#include "kernel/circulation.h"

namespace AqualinkAutomate::Scheduling { struct ControllerSchedule; }

namespace AqualinkAutomate::Interfaces
{

	class ICommandDispatcher
	{
	public:
		enum class CommandResult
		{
			Success,
			DeviceNotFound,
			NoSerialAdapter,
			UnknownEquipmentType,
			InvalidValue
		};

		enum class DeviceAction
		{
			On,
			Off,
			Toggle
		};

		virtual ~ICommandDispatcher() = default;

		virtual CommandResult ToggleByUuid(const boost::uuids::uuid& uuid) = 0;
		virtual CommandResult ToggleByLabel(const std::string& label) = 0;
		virtual CommandResult CommandByUuid(const boost::uuids::uuid& uuid, DeviceAction action) = 0;
		virtual CommandResult CommandByLabel(const std::string& label, DeviceAction action) = 0;
		virtual CommandResult SetPoolSetpoint(uint8_t temperature) = 0;
		virtual CommandResult SetSpaSetpoint(uint8_t temperature) = 0;
		virtual CommandResult SetChlorinatorPercentage(uint8_t percentage) = 0;
		virtual CommandResult SetChlorinatorBoost(bool enable) = 0;
		virtual CommandResult SetCirculationMode(Kernel::CirculationModes mode) = 0;
		// Enable/disable a heater identified by its body of water (Pool/Spa heater, or Shared for
		// the solar heater).
		virtual CommandResult SetHeaterMode(Kernel::BodyOfWaterIds heater_body, bool enable) = 0;
		virtual CommandResult SelectIAQPageButton(uint8_t button_index) = 0;

		// Create / delete one of the CONTROLLER's own internal programs (its built-in timers) by
		// driving a capable panel's Program menu. Unlike the point actions above these are SPANS and
		// run asynchronously over the bus, so Success means "accepted / queued" -- the caller polls
		// /api/controller/schedules to observe the result. Routed to a ControllerScheduleWriter.
		virtual CommandResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) = 0;
		virtual CommandResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) = 0;
		// Edit an existing controller program in place: `existing` locates the current row, `desired`
		// carries the new field values it is changed to. Async like the create/delete above.
		virtual CommandResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
