#pragma once

#include <cstdint>

#include "devices/capabilities/actuation_types.h"
#include "kernel/body_of_water_ids.h"

namespace AqualinkAutomate::Devices::Capabilities
{

	// Mixin advertised by any controller that can drive a salt-water generator
	// (chlorinator) output percentage and boost mode. Both the AqualinkTouch (IAQ, 0x33,
	// direct value-submit) and the OneTouch (0x40-43, via the Set AquaPure menu) can carry
	// chlorinator control, so the CommandDispatcher selects the highest ControllerPriority()
	// instance present (the IAQ's direct channel outranks the OneTouch menu-nav).
	class ChlorinatorController
	{
	public:
		virtual ~ChlorinatorController() = default;

		// Set the output for ONE body of water. The panel keeps a SEPARATE setpoint per body --
		// pool and spa are independent, not one value applied to whichever body is circulating --
		// so the body is part of the command, never inferred.
		virtual ActuationResult SetChlorinatorPercentage(uint8_t percentage, Kernel::BodyOfWaterIds body) = 0;
		virtual ActuationResult SetChlorinatorBoost(bool enable) = 0;

		// Precedence when several ChlorinatorControllers are connected at once.
		virtual ActuationPriority ControllerPriority() const = 0;
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
