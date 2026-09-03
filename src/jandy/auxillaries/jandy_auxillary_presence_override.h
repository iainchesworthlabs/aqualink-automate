#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

#include "auxillaries/jandy_auxillary_id.h"

namespace AqualinkAutomate::Kernel
{
	class DevicesGraph;
}
// namespace AqualinkAutomate::Kernel

namespace AqualinkAutomate::Auxillaries
{

	// Operator override of live aux presence detection. "Auto" (the default, no entry in the
	// overrides map) means live detection decides; "Present"/"Absent" is the operator overruling
	// it -- e.g. because a real aux keeps failing to reply on the wire, or because a phantom
	// keeps getting recreated. This is NOT a relay on/off control: it only decides whether the
	// slot is treated as existing at all. See PreferencesHub::AuxPresenceOverrides.
	enum class AuxPresenceOverride
	{
		Auto,
		Present,
		Absent
	};

	// Read the override for `id` out of the raw PreferencesHub::AuxPresenceOverrides blob, keyed
	// by the aux's canonical/hardware label (magic_enum::enum_name(id), e.g. "Aux5"). Anything
	// other than the literal strings "present"/"absent" (missing key, wrong type, unrecognised
	// value) resolves to Auto -- an override is only ever an explicit operator choice.
	AuxPresenceOverride GetPresenceOverride(JandyAuxillaryIds id, const nlohmann::json& overrides);

	inline bool IsForcedAbsent(JandyAuxillaryIds id, const nlohmann::json& overrides)
	{
		return AuxPresenceOverride::Absent == GetPresenceOverride(id, overrides);
	}

	inline bool IsForcedPresent(JandyAuxillaryIds id, const nlohmann::json& overrides)
	{
		return AuxPresenceOverride::Present == GetPresenceOverride(id, overrides);
	}

	// Reconcile the device graph against `overrides`: synthesize a device (via the same
	// no-wire-evidence factory call the Label-Aux scrape uses) for every Present entry that has
	// no live device yet, and remove the device for every Absent entry that has one. Auto entries
	// are left untouched -- they carry no opinion, live detection alone decides. Returns the
	// number of devices created or removed.
	std::size_t ApplyPresenceOverrides(Kernel::DevicesGraph& devices, const nlohmann::json& overrides);

}
// namespace AqualinkAutomate::Auxillaries
