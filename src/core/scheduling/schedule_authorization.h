#pragma once

#include <string>

#include "auth/subject.h"
#include "scheduling/schedule.h"

namespace AqualinkAutomate::Kernel { class DataHub; }

namespace AqualinkAutomate::Scheduling
{

	//=========================================================================
	// Schedule action-gating (docs/auth-redesign.md §4, D14).
	//
	// A schedule performs a control action when it fires, so saving one is
	// only permitted when the SAVING subject is itself entitled to that action
	// — otherwise a subject could escalate by scheduling what they cannot do
	// directly.  This runs the SAME PolicyEngine decision the live control
	// route would make:
	//
	//   button on/off/toggle -> equipment.control.aux, scoped to the target
	//                           device's id (resolved from its label via the
	//                           DataHub, matching /api/equipment/buttons/{id});
	//                           an unresolvable label fails CLOSED
	//   pool/spa setpoint     -> equipment.control.setpoints
	//   chlorinator percent   -> equipment.control.chlorinator
	//   circulation mode      -> equipment.control.circulation
	//
	// `auth_enabled` mirrors the routing posture: with the identity system off
	// the PolicyEngine permits everything, so this is a no-op (historical
	// behaviour preserved).  On denial returns false and sets `error`.
	//=========================================================================
	bool AuthorizeScheduleAction(const Auth::Subject& subject, const Schedule& schedule, const Kernel::DataHub& data_hub, bool auth_enabled, std::string& error);

}
// namespace AqualinkAutomate::Scheduling
