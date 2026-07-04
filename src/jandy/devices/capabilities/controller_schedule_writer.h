#pragma once

#include "devices/capabilities/actuation_types.h"

namespace AqualinkAutomate::Scheduling { struct ControllerSchedule; }

namespace AqualinkAutomate::Devices::Capabilities
{

	// Mixin advertised by a controller that can CREATE or DELETE its own internal program entries
	// (the controller's built-in schedule timers) by driving its Program menu. The AqualinkTouch
	// (IAQDevice, 0x33) is the implementer; the OneTouch and a Serial Adapter do not offer this
	// write path, so a request falls through to NotSupported on them. The write is asynchronous
	// (a page-navigation goal serviced per poll), so Accepted means "queued", not "done".
	class ControllerScheduleWriter
	{
	public:
		virtual ~ControllerScheduleWriter() = default;

		virtual ActuationResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) = 0;
		virtual ActuationResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) = 0;

		// Precedence when several writers are connected at once (only the IAQ implements this today).
		virtual ActuationPriority ControllerPriority() const = 0;
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
