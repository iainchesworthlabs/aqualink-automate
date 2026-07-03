#pragma once

#include <array>
#include <string_view>

namespace AqualinkAutomate::Auth::Vocabulary
{

	//=========================================================================
	// The v1 entitlement action vocabulary (docs/auth-redesign.md §4).
	//
	// Actions are plain strings on the wire and in the stores — this header is
	// the canonical list used by route declarations, the admin UI/API, and
	// validation.  The selector grammar ("action[:selector]") means finer grain
	// can be added later without changing this vocabulary's shape.
	//=========================================================================

	// Superuser: manage users, groups, entitlements, API keys, auth config and
	// system preferences.  The PolicyEngine treats a subject holding this as
	// permitted for EVERY action (see policy_engine.h).
	inline constexpr std::string_view SYSTEM_ADMIN{ "system.admin" };

	// Read equipment state, temperatures, chemistry, history (binary in v1).
	inline constexpr std::string_view EQUIPMENT_VIEW{ "equipment.view" };

	// Control actions; AUX supports per-resource selectors (aux id).
	inline constexpr std::string_view EQUIPMENT_CONTROL_AUX{ "equipment.control.aux" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_HEATER{ "equipment.control.heater" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_SETPOINTS{ "equipment.control.setpoints" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_CIRCULATION{ "equipment.control.circulation" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_CHLORINATOR{ "equipment.control.chlorinator" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_IAQ{ "equipment.control.iaq" };
	inline constexpr std::string_view EQUIPMENT_CONTROL_SPASIDE{ "equipment.control.spaside" };

	inline constexpr std::string_view SCHEDULES_VIEW{ "schedules.view" };
	inline constexpr std::string_view SCHEDULES_EDIT{ "schedules.edit" };

	inline constexpr std::string_view DIAGNOSTICS_VIEW{ "diagnostics.view" };

	// Manage one's own preferences (implicit for authenticated subjects).
	inline constexpr std::string_view PREFS_SELF{ "prefs.self" };

	// The full v1 vocabulary, for admin UI enumeration and store validation.
	inline constexpr std::array ALL_ACTIONS{
		SYSTEM_ADMIN,
		EQUIPMENT_VIEW,
		EQUIPMENT_CONTROL_AUX,
		EQUIPMENT_CONTROL_HEATER,
		EQUIPMENT_CONTROL_SETPOINTS,
		EQUIPMENT_CONTROL_CIRCULATION,
		EQUIPMENT_CONTROL_CHLORINATOR,
		EQUIPMENT_CONTROL_IAQ,
		EQUIPMENT_CONTROL_SPASIDE,
		SCHEDULES_VIEW,
		SCHEDULES_EDIT,
		DIAGNOSTICS_VIEW,
		PREFS_SELF
	};

	// True when `action` is part of the known vocabulary.  Stores accept only
	// known actions so typos cannot silently create dead grants.
	bool IsKnownAction(std::string_view action);

}
// namespace AqualinkAutomate::Auth::Vocabulary
