#pragma once

#include <string>

namespace AqualinkAutomate::Kernel
{

	enum class SystemBoards
	{
		RS4_Only,
		RS6_Only,
		RS8_Only,

		RS2_6_Dual,
		RS2_10_Dual,
		RS2_14_Dual,
		RS2_22_Dual,
		RS2_30_Dual,

		RS4_Combo,
		RS6_Combo,
		RS8_Combo,
		RS12_Combo,
		RS16_Combo,
		RS24_Combo,
		RS32_Combo,

		PD4_Only,
		PD8_Only,
		PD4_Combo,
		PD6_Combo,
		PD8_Combo,

		Unknown
	};

	// Human-readable label for a system board (e.g. RS8_Combo -> "RS-8 Combo").
	// The strings match the canonical panel-type keys decoded by
	// Utility::PoolConfigurationDecoder so a label round-trips back to the enum.
	// Use this ONLY for user-facing display (e.g. the diagnostics page). The raw
	// magic_enum name is retained where the value is a machine token, i.e. the
	// equipment-cache snapshot (restored via enum_cast) and the cache fingerprint.
	std::string ToDisplayString(SystemBoards board);

}
// namespace AqualinkAutomate::Kernel
