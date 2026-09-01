#include <boost/test/unit_test.hpp>

#include <optional>

#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/auxillary_devices/pump_status.h"

using namespace AqualinkAutomate::Kernel;

BOOST_AUTO_TEST_SUITE(KernelStatusConversions_TestSuite)

// =============================================================================
// ConvertToChlorinatorStatus
// =============================================================================

BOOST_AUTO_TEST_CASE(TestChlorinatorStatus_On)
{
	auto result = ConvertToChlorinatorStatus(AuxillaryStatuses::On);
	BOOST_CHECK(result == ChlorinatorStatuses::On);
}

BOOST_AUTO_TEST_CASE(TestChlorinatorStatus_Off)
{
	auto result = ConvertToChlorinatorStatus(AuxillaryStatuses::Off);
	BOOST_CHECK(result == ChlorinatorStatuses::Off);
}

BOOST_AUTO_TEST_CASE(TestChlorinatorStatus_Enabled_MapsToUnknown)
{
	auto result = ConvertToChlorinatorStatus(AuxillaryStatuses::Enabled);
	BOOST_CHECK(result == ChlorinatorStatuses::Unknown);
}

BOOST_AUTO_TEST_CASE(TestChlorinatorStatus_Pending_MapsToUnknown)
{
	auto result = ConvertToChlorinatorStatus(AuxillaryStatuses::Pending);
	BOOST_CHECK(result == ChlorinatorStatuses::Unknown);
}

BOOST_AUTO_TEST_CASE(TestChlorinatorStatus_Unknown_MapsToUnknown)
{
	auto result = ConvertToChlorinatorStatus(AuxillaryStatuses::Unknown);
	BOOST_CHECK(result == ChlorinatorStatuses::Unknown);
}

// =============================================================================
// ConvertToHeaterStatus
// =============================================================================

BOOST_AUTO_TEST_CASE(TestHeaterStatus_On_MapsToHeating)
{
	auto result = ConvertToHeaterStatus(AuxillaryStatuses::On);
	BOOST_CHECK(result == HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(TestHeaterStatus_Off)
{
	auto result = ConvertToHeaterStatus(AuxillaryStatuses::Off);
	BOOST_CHECK(result == HeaterStatuses::Off);
}

BOOST_AUTO_TEST_CASE(TestHeaterStatus_Enabled)
{
	auto result = ConvertToHeaterStatus(AuxillaryStatuses::Enabled);
	BOOST_CHECK(result == HeaterStatuses::Enabled);
}

BOOST_AUTO_TEST_CASE(TestHeaterStatus_Pending_MapsToUnknown)
{
	auto result = ConvertToHeaterStatus(AuxillaryStatuses::Pending);
	BOOST_CHECK(result == HeaterStatuses::Unknown);
}

BOOST_AUTO_TEST_CASE(TestHeaterStatus_Unknown_MapsToUnknown)
{
	auto result = ConvertToHeaterStatus(AuxillaryStatuses::Unknown);
	BOOST_CHECK(result == HeaterStatuses::Unknown);
}

// =============================================================================
// ConvertToPumpStatus
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPumpStatus_On_MapsToRunning)
{
	auto result = ConvertToPumpStatus(AuxillaryStatuses::On);
	BOOST_CHECK(result == PumpStatuses::Running);
}

BOOST_AUTO_TEST_CASE(TestPumpStatus_Off)
{
	auto result = ConvertToPumpStatus(AuxillaryStatuses::Off);
	BOOST_CHECK(result == PumpStatuses::Off);
}

BOOST_AUTO_TEST_CASE(TestPumpStatus_Enabled_MapsToUnknown)
{
	auto result = ConvertToPumpStatus(AuxillaryStatuses::Enabled);
	BOOST_CHECK(result == PumpStatuses::Unknown);
}

BOOST_AUTO_TEST_CASE(TestPumpStatus_Pending_MapsToUnknown)
{
	auto result = ConvertToPumpStatus(AuxillaryStatuses::Pending);
	BOOST_CHECK(result == PumpStatuses::Unknown);
}

BOOST_AUTO_TEST_CASE(TestPumpStatus_Unknown_MapsToUnknown)
{
	auto result = ConvertToPumpStatus(AuxillaryStatuses::Unknown);
	BOOST_CHECK(result == PumpStatuses::Unknown);
}

// =============================================================================
// ResolveGeneratingReason
//
// A chlorinator's instantaneous output is 0% whenever the cell is idle, which on
// its own reads as "off or broken" -- for most of the day, on a system whose
// filter pump runs on a schedule, that is simply wrong. These cases pin which
// explanation each combination of (output, setpoint, health, pump) produces.
// =============================================================================

namespace
{
	ChlorinatorGeneratingContext MakeContext(
		std::optional<uint8_t> generating,
		std::optional<uint8_t> setpoint,
		ChlorinatorHealth health = ChlorinatorHealth::Ok,
		std::optional<bool> pump_running = std::nullopt)
	{
		ChlorinatorGeneratingContext context;
		context.GeneratingPercent = generating;
		context.SetpointPercent = setpoint;
		context.Health = health;
		context.FilterPumpRunning = pump_running;
		return context;
	}
}
// unnamed namespace

BOOST_AUTO_TEST_CASE(TestGeneratingReason_ProducingOutput_IsGenerating)
{
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(50, 50, ChlorinatorHealth::Ok, true)) == ChlorinatorGeneratingReason::Generating);

	// Producing wins over every other signal: whatever else is reported, the cell IS working.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(20, 0, ChlorinatorHealth::Warning_LowSalt, false)) == ChlorinatorGeneratingReason::Generating);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_ZeroSetpoint_IsOff)
{
	// Deliberately turned off -- 0% is the whole story.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 0, ChlorinatorHealth::Ok, true)) == ChlorinatorGeneratingReason::Off);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_ConfiguredButPumpStopped_IsPumpOff)
{
	// THE reported case: configured to 50%, cell healthy, filter pump off. Output is
	// legitimately 0 and the chlorinator is fine.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Ok, false)) == ChlorinatorGeneratingReason::PumpOff);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_CellReportsNoFlow_PrefersNoFlowOverPumpOff)
{
	// The cell's own no-flow report is more specific than our inference from the pump, so it
	// wins even when the pump also reads stopped.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Warning_NoFlow, false)) == ChlorinatorGeneratingReason::NoFlow);
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Warning_NoFlow, true)) == ChlorinatorGeneratingReason::NoFlow);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_FaultedCellWithPumpRunning_IsFault)
{
	// Pump running and configured to produce, but the cell is unhappy -- that IS the reason.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Warning_LowSalt, true)) == ChlorinatorGeneratingReason::Fault);
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Error_CheckPCB, true)) == ChlorinatorGeneratingReason::Fault);
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::GeneralFault, true)) == ChlorinatorGeneratingReason::Fault);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_ConfiguredAndHealthyWithNoExplanation_IsIdle)
{
	// Pump running, cell healthy, still not producing: say "idle" rather than inventing a cause.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Ok, true)) == ChlorinatorGeneratingReason::Idle);

	// No pump discovered at all -> we cannot blame a pump we have never seen.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, 50, ChlorinatorHealth::Ok, std::nullopt)) == ChlorinatorGeneratingReason::Idle);
}

BOOST_AUTO_TEST_CASE(TestGeneratingReason_NoSetpointKnown_IsUnknown)
{
	// Nothing scraped yet: "off" would be a guess, and a wrong one on a system whose setpoint
	// simply has not been read.
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(0, std::nullopt, ChlorinatorHealth::Unknown, false)) == ChlorinatorGeneratingReason::Unknown);
	BOOST_CHECK(ResolveGeneratingReason(MakeContext(std::nullopt, std::nullopt)) == ChlorinatorGeneratingReason::Unknown);
}

BOOST_AUTO_TEST_SUITE_END()
