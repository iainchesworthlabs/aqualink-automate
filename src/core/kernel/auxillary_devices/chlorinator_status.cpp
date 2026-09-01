#include "kernel/auxillary_devices/chlorinator_status.h"

namespace AqualinkAutomate::Kernel
{

	ChlorinatorStatuses ConvertToChlorinatorStatus(Kernel::AuxillaryStatuses aux_status)
	{
		switch (aux_status)
		{
		case AuxillaryStatuses::On:
			return ChlorinatorStatuses::On;

		case AuxillaryStatuses::Off:
			return ChlorinatorStatuses::Off;

		case AuxillaryStatuses::Enabled:
			[[fallthrough]];
		case AuxillaryStatuses::Pending:
			[[fallthrough]];
		case AuxillaryStatuses::Unknown:
			[[fallthrough]];
		default:
			return ChlorinatorStatuses::Unknown;
		}
	}

	namespace
	{
		bool IsFaulted(ChlorinatorHealth health)
		{
			using enum ChlorinatorHealth;

			switch (health)
			{
			case Warning_LowSalt:
			case Warning_HighSalt:
			case Warning_HighCurrent:
			case Warning_CleanCell:
			case Warning_LowVoltage:
			case Warning_LowTemperature:
			case Error_CheckPCB:
			case GeneralFault:
				return true;

			// Warning_NoFlow is a REASON of its own (reported below), not a generic fault.
			case Warning_NoFlow:
			case Ok:
			case TurningOff:
			case Unknown:
			default:
				return false;
			}
		}
	}
	// unnamed namespace

	ChlorinatorGeneratingReason ResolveGeneratingReason(const ChlorinatorGeneratingContext& context)
	{
		// Producing: whatever else is true, the cell is working.
		if (context.GeneratingPercent.value_or(0) > 0)
		{
			return ChlorinatorGeneratingReason::Generating;
		}

		if (!context.SetpointPercent.has_value())
		{
			// No output and no configured target: nothing has been scraped yet, so we cannot
			// distinguish "switched off" from "not yet known". Say so rather than implying off.
			return ChlorinatorGeneratingReason::Unknown;
		}

		if (0 == context.SetpointPercent.value())
		{
			return ChlorinatorGeneratingReason::Off;
		}

		// From here the cell IS configured to produce but is not producing. Order matters: the
		// cell's own no-flow report is more specific than our inference from the pump, and a
		// stopped pump explains the silence better than a generic fault would.
		if (ChlorinatorHealth::Warning_NoFlow == context.Health)
		{
			return ChlorinatorGeneratingReason::NoFlow;
		}

		if (context.FilterPumpRunning.has_value() && !context.FilterPumpRunning.value())
		{
			return ChlorinatorGeneratingReason::PumpOff;
		}

		if (IsFaulted(context.Health))
		{
			return ChlorinatorGeneratingReason::Fault;
		}

		return ChlorinatorGeneratingReason::Idle;
	}

}
// namespace AqualinkAutomate::Kernel
