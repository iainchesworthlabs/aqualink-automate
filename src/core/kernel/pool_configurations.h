#pragma once

#include <string>

namespace AqualinkAutomate::Kernel
{

	enum class PoolConfigurations
	{
		SingleBody,
		DualBody_SharedEquipment,
		DualBody_DualEquipment,
		Unknown
	};

	// Human-readable label for a pool configuration (e.g. DualBody_SharedEquipment
	// -> "Dual Body (Shared Equipment)"). Use this ONLY for user-facing display
	// (e.g. the diagnostics page). The raw magic_enum name is retained where the
	// value is a machine token, i.e. the equipment-cache snapshot (restored via
	// enum_cast), the cache fingerprint, and the frontend 'Unknown' readiness check
	// on the /api/equipment payload.
	std::string ToDisplayString(PoolConfigurations config);

}
// namespace AqualinkAutomate::Kernel
