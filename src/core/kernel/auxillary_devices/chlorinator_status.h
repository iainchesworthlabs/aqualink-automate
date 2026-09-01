#pragma once

#include <cstdint>
#include <optional>

#include "kernel/auxillary_devices/auxillary_status.h"

namespace AqualinkAutomate::Kernel
{

	/// Operating state of the chlorinator (on/off).
	enum class ChlorinatorStatuses : uint8_t
	{
		Off,
		On,
		Unknown
	};

	/// Health/diagnostic status reported by the AquaRite cell.
	enum class ChlorinatorHealth : uint8_t
	{
		Ok,
		TurningOff,
		Warning_NoFlow,
		Warning_LowSalt,
		Warning_HighSalt,
		Warning_HighCurrent,
		Warning_CleanCell,
		Warning_LowVoltage,
		Warning_LowTemperature,
		Error_CheckPCB,
		GeneralFault,
		Unknown
	};

	/// Why the cell is (or is not) producing chlorine right now.
	///
	/// The instantaneous output is 0% whenever the cell is idle, which reads as "the chlorinator
	/// is off or broken" even when it is configured, healthy, and simply waiting for the filter
	/// pump. Reporting the REASON alongside the number lets every consumer -- the web UI, MQTT
	/// and Home Assistant -- say which of those it is instead of each re-deriving it (or not).
	enum class ChlorinatorGeneratingReason : uint8_t
	{
		Generating,    ///< producing chlorine
		Off,           ///< configured output is 0% -- deliberately turned off
		PumpOff,       ///< configured to produce, but no filter pump is running (no flow past the cell)
		NoFlow,        ///< configured to produce and a pump is running, but the cell reports no flow
		Fault,         ///< configured to produce but the cell is in a warning/error state
		Idle,          ///< configured to produce and not generating, with no reason we can name
		Unknown        ///< not enough information yet (no output and no setpoint reported)
	};

	/// Inputs for ResolveGeneratingReason. Every field is optional because each arrives from a
	/// different source at a different time; the resolver degrades to Unknown rather than guessing.
	struct ChlorinatorGeneratingContext
	{
		std::optional<uint8_t> GeneratingPercent;   ///< instantaneous output
		std::optional<uint8_t> SetpointPercent;     ///< configured output target for the active body
		ChlorinatorHealth Health{ ChlorinatorHealth::Unknown };
		std::optional<bool> FilterPumpRunning;      ///< nullopt when no filter pump has been discovered
	};

	ChlorinatorStatuses ConvertToChlorinatorStatus(AuxillaryStatuses aux_states);

	ChlorinatorGeneratingReason ResolveGeneratingReason(const ChlorinatorGeneratingContext& context);

}
// namespace AqualinkAutomate::Kernel
