#pragma once

#include "devices/capabilities/actuation_types.h"

namespace AqualinkAutomate::Scheduling { struct ControllerSchedule; }

namespace AqualinkAutomate::Devices::Capabilities
{

	// Mixin advertised by a controller that can CREATE, DELETE or EDIT its own internal program
	// entries (the controller's built-in schedule timers) by driving its Program menu. Two devices
	// implement it: the AqualinkTouch (IAQDevice, 0x33) drives its touch-grid Program pages, and the
	// OneTouch (0x40) drives its 16x12 character Program menu with discrete nav keys (see
	// docs/onetouch_schedule_protocol.md, write path). A Serial Adapter does not offer this write
	// path, so a request falls through to NotSupported on it. Only an EMULATED panel transmits, so a
	// passive (real-observed) IAQ/OneTouch also reports NotSupported and the dispatcher falls back to
	// the other. The write is asynchronous (a page-navigation goal serviced per poll), so Accepted
	// means "queued", not "done".
	class ControllerScheduleWriter
	{
	public:
		virtual ~ControllerScheduleWriter() = default;

		virtual ActuationResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) = 0;
		virtual ActuationResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) = 0;

		// EDIT an existing controller program: locate the row matching `existing` (target + day +
		// on/off times), enter its edit mode, and change its fields to `desired`. Accepted == queued.
		virtual ActuationResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) = 0;

		// Precedence when several writers are connected at once (only the IAQ implements this today).
		virtual ActuationPriority ControllerPriority() const = 0;
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
