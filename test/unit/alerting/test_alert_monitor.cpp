#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>

#include "alerting/alert_condition.h"
#include "alerting/alert_monitor.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"
#include "options/options_alerting_options.h"
#include "types/units_dimensionless.h"
#include "jandy/messages/jandy_message_ids.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Alerting;

namespace
{
	// Records every transition the monitor emits so a test can assert the edge
	// sequence (and prove latching suppresses repeats).
	struct SinkRecorder
	{
		std::vector<AlertTransition> transitions;

		AlertMonitor::Sink AsSink()
		{
			return [this](const AlertTransition& t) { transitions.push_back(t); };
		}

		std::size_t CountFor(std::string_view key) const
		{
			std::size_t n = 0;
			for (const auto& t : transitions) { if (t.condition == key) { ++n; } }
			return n;
		}
	};

	// The evaluators read ChlorinatorHealthFlagsTrait (the wire status byte is a true
	// bitfield, so more than one flag can be active at once); ChlorinatorHealthTrait
	// remains the single worst-of-the-set value. Tests that only care about a single
	// flag being active set both traits to the same one-element state via this helper,
	// so the two can never (accidentally) disagree.
	void SetHealth(const std::shared_ptr<Kernel::AuxillaryDevice>& chlor, Kernel::ChlorinatorHealth health)
	{
		using namespace Kernel::AuxillaryTraitsTypes;
		chlor->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, health);
		chlor->AuxillaryTraits.Set(ChlorinatorHealthFlagsTrait{}, std::set<Kernel::ChlorinatorHealth>{ health });
	}

	std::shared_ptr<Kernel::AuxillaryDevice> MakeChlorinator(Kernel::ChlorinatorHealth health)
	{
		using namespace Kernel::AuxillaryTraitsTypes;
		auto chlor = std::make_shared<Kernel::AuxillaryDevice>();
		chlor->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlor->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlor->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		SetHealth(chlor, health);
		return chlor;
	}
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_AlertMonitor, Test::HubLocatorInjector)

// salt_low raises below the threshold and only clears after +100 ppm hysteresis,
// so a reading hovering at the boundary cannot flap.
BOOST_AUTO_TEST_CASE(SaltLow_RaisesBelowThreshold_ClearsWithHysteresis)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;
	settings.salt_low_ppm = 2600;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	// AlertMonitor reads the threshold live from PreferencesHub (seeded from the
	// CLI in production); set it explicitly in the test.
	Find<Kernel::PreferencesHub>()->AlertSaltLowPpm = 2600;

	auto data_hub = Find<Kernel::DataHub>();

	// Below threshold -> raise, carrying the structured values behind the
	// detail prose (the UI's translated alert text and webhook automations
	// consume these instead of parsing English).
	data_hub->SaltLevel(2000 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 1u);
	BOOST_CHECK(rec.transitions.back().raised);
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::SaltLow));
	BOOST_CHECK_EQUAL(2000.0, rec.transitions.back().params.at("salt_ppm").get<double>());
	BOOST_CHECK_EQUAL(2600.0, rec.transitions.back().params.at("threshold_ppm").get<double>());

	// Inside the hysteresis band (>= threshold but < threshold+100) -> stay raised.
	data_hub->SaltLevel(2650 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 1u); // no new transition
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::SaltLow));

	// Above threshold + 100 -> clear.
	data_hub->SaltLevel(2750 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 2u);
	BOOST_CHECK(!rec.transitions.back().raised);
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SaltLow));
}

// salt_low_ppm == 0 disables the salt check entirely.
BOOST_AUTO_TEST_CASE(SaltLow_Disabled_NeverRaises)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;
	settings.salt_low_ppm = 0;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::PreferencesHub>()->AlertSaltLowPpm = 0;   // disables the check
	Find<Kernel::DataHub>()->SaltLevel(500 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 0u);
}

// chlorinator_fault latches on a hard fault and clears on recovery.
BOOST_AUTO_TEST_CASE(ChlorinatorFault_RaisesOnGeneralFault_ClearsOnOk)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinator(Kernel::ChlorinatorHealth::Ok);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorFault();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));

	SetHealth(chlor, Kernel::ChlorinatorHealth::GeneralFault);
	monitor.EvaluateChlorinatorFault();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorFault), 1u);

	SetHealth(chlor, Kernel::ChlorinatorHealth::Ok);
	monitor.EvaluateChlorinatorFault();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorFault), 2u);
}

// chlorinator_warning raises on ANY cell warning (not just low salt), names the
// specific warning in the detail, and clears on recovery — without tripping the
// hard-fault condition.
BOOST_AUTO_TEST_CASE(ChlorinatorWarning_RaisesOnWarning_NamesIt_ClearsOnOk)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinator(Kernel::ChlorinatorHealth::Ok);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));

	// A No-Flow warning (NOT low salt) must still be surfaced and named.
	SetHealth(chlor, Kernel::ChlorinatorHealth::Warning_NoFlow);
	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));   // a warning is not a fault
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 1u);
	BOOST_CHECK(rec.transitions.back().detail.find("No flow") != std::string::npos);
	// The params carry the raw health enum name so the UI maps it to its own
	// translated label (swg_health.* catalog keys) instead of parsing the prose.
	BOOST_CHECK_EQUAL("Warning_NoFlow", rec.transitions.back().params.at("health").get<std::string>());

	// Recovery clears it.
	SetHealth(chlor, Kernel::ChlorinatorHealth::Ok);
	monitor.EvaluateChlorinatorWarning();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 2u);
}

// A hard fault raises chlorinator_fault but NOT chlorinator_warning, so the two
// severities stay distinct; the fault detail names the specific fault.
BOOST_AUTO_TEST_CASE(ChlorinatorWarning_NotRaisedByHardFault)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinator(Kernel::ChlorinatorHealth::Error_CheckPCB);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	BOOST_CHECK(rec.transitions.back().detail.find("Check PCB") != std::string::npos);
}

// Every catalogued cell WARNING (not just No-Flow / low salt) is surfaced and
// named in the detail — this exercises the full ChlorinatorWarningLabel map.
BOOST_AUTO_TEST_CASE(ChlorinatorWarning_NamesEveryCatalogWarning)
{
	struct Case { Kernel::ChlorinatorHealth health; const char* text; };
	const Case cases[] = {
		{ Kernel::ChlorinatorHealth::Warning_LowSalt,        "Low salt" },
		{ Kernel::ChlorinatorHealth::Warning_HighSalt,       "High salt" },
		{ Kernel::ChlorinatorHealth::Warning_HighCurrent,    "High current" },
		{ Kernel::ChlorinatorHealth::Warning_CleanCell,      "Clean cell" },
		{ Kernel::ChlorinatorHealth::Warning_LowVoltage,     "Low voltage" },
		{ Kernel::ChlorinatorHealth::Warning_LowTemperature, "Low temperature" },
	};

	for (const auto& c : cases)
	{
		boost::asio::io_context io;
		Options::Alerting::AlertingSettings settings;

		AlertMonitor monitor(io, *this, settings);
		SinkRecorder rec;
		monitor.AddSink(rec.AsSink());

		auto data_hub = Find<Kernel::DataHub>();
		auto chlor = MakeChlorinator(c.health);
		data_hub->Devices.Add(chlor);

		monitor.EvaluateChlorinatorWarning();
		BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
		BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 1u);
		BOOST_CHECK_MESSAGE(rec.transitions.back().detail.find(c.text) != std::string::npos,
			std::string{ "warning detail should name: " } + c.text);

		// The DataHub is shared across this fixture instance: drop the device so the
		// next case evaluates ITS chlorinator alone rather than the first-added one.
		data_hub->Devices.Remove(chlor);
	}
}

// The wire status byte is a true bitfield, so a hard fault and a warning can be
// active simultaneously (e.g. a check-PCB fault reported alongside a low-salt
// warning). Both conditions must raise at once - previously impossible, since both
// evaluators read the same single ChlorinatorHealthTrait value and could therefore
// only ever match one predicate per poll.
BOOST_AUTO_TEST_CASE(ChlorinatorWarningAndFault_BothRaiseSimultaneously)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinator(Kernel::ChlorinatorHealth::Ok);
	data_hub->Devices.Add(chlor);

	// Wire byte 0x82 = Warning_LowSalt | Error_CheckPCB.
	using namespace Kernel::AuxillaryTraitsTypes;
	chlor->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Error_CheckPCB);
	chlor->AuxillaryTraits.Set(ChlorinatorHealthFlagsTrait{}, std::set<Kernel::ChlorinatorHealth>{
		Kernel::ChlorinatorHealth::Warning_LowSalt, Kernel::ChlorinatorHealth::Error_CheckPCB });

	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorFault));

	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 1u);
	BOOST_CHECK(rec.transitions.back().detail.find("Check PCB") != std::string::npos);

	// Each condition's params.health names the flag relevant to THAT condition (not
	// the other one), so a warning's params never point at an unrelated fault name.
	auto warning_transition = std::find_if(rec.transitions.begin(), rec.transitions.end(),
		[](const AlertTransition& t) { return t.condition == ConditionKeys::ChlorinatorWarning; });
	BOOST_REQUIRE(warning_transition != rec.transitions.end());
	BOOST_CHECK_EQUAL("Warning_LowSalt", warning_transition->params.at("health").get<std::string>());

	auto fault_transition = std::find_if(rec.transitions.begin(), rec.transitions.end(),
		[](const AlertTransition& t) { return t.condition == ConditionKeys::ChlorinatorFault; });
	BOOST_REQUIRE(fault_transition != rec.transitions.end());
	BOOST_CHECK_EQUAL("Error_CheckPCB", fault_transition->params.at("health").get<std::string>());
}

// Start() is idempotent: a second call while already running is a no-op (does not
// re-subscribe or re-baseline).
BOOST_AUTO_TEST_CASE(Start_IsIdempotent)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);

	std::int64_t now = 2000;
	monitor.SetClock([&now] { return now; });

	monitor.Start();
	BOOST_CHECK_NO_THROW(monitor.Start());   // second Start takes the already-running early return
	monitor.Stop();

	// Stop() is likewise a no-op once already stopped.
	BOOST_CHECK_NO_THROW(monitor.Stop());
}

// salt_low never raises on absent data: a salt reading of exactly 0 means "no
// sample yet", so the check returns before evaluating the threshold.
BOOST_AUTO_TEST_CASE(SaltLow_ZeroReading_NeverRaises)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;
	settings.salt_low_ppm = 2600;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::PreferencesHub>()->AlertSaltLowPpm = 2600;

	// No salt sample yet (0 ppm) -> absent-data guard fires; no alert despite the
	// threshold being 2600.
	Find<Kernel::DataHub>()->SaltLevel(0.0 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SaltLow));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 0u);
}

// service_mode tracks the DataHub equipment mode.
BOOST_AUTO_TEST_CASE(ServiceMode_TracksEquipmentMode)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();

	data_hub->Mode = Kernel::EquipmentMode::Service;
	monitor.EvaluateServiceMode();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ServiceMode));

	data_hub->Mode = Kernel::EquipmentMode::Normal;
	monitor.EvaluateServiceMode();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ServiceMode));

	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ServiceMode), 2u);
}

// serial_comms_loss raises after the timeout elapses with no new messages and
// clears as soon as traffic resumes (deterministic via an injected clock).
BOOST_AUTO_TEST_CASE(SerialCommsLoss_RaisesAfterTimeout_ClearsOnTraffic)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;
	settings.comms_timeout_seconds = 60;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::PreferencesHub>()->AlertCommsTimeoutSeconds = 60;

	std::int64_t now = 1000;
	monitor.SetClock([&now] { return now; });

	auto stats = Find<Kernel::StatisticsHub>();

	// Start establishes the baseline at t=1000 with zero messages.
	monitor.Start();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SerialCommsLoss));

	// Timeout elapses with no new traffic -> raise.
	now = 1000 + 60;
	monitor.EvaluateSerialCommsLoss();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::SerialCommsLoss));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SerialCommsLoss), 1u);

	// New traffic -> clear.
	stats->MessageCounts[Messages::JandyMessageIds::AQUARITE_Percent] += 1u;
	now = 1000 + 70;
	monitor.EvaluateSerialCommsLoss();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SerialCommsLoss));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SerialCommsLoss), 2u);

	monitor.Stop();
}

// temperature_stale raises only for the UNEXPECTED flavour of staleness: the
// pump is running (so the active body's sensor should be reporting) yet the
// reading is older than the staleness threshold.  Pump-off staleness is the
// nightly normal — the UI ages the reading quietly and no alert may fire.
BOOST_AUTO_TEST_CASE(TemperatureStale_RaisesOnlyWhilePumpRunning)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();

	// A filter pump, currently running.
	auto pump = std::make_shared<Kernel::AuxillaryDevice>();
	{
		using namespace Kernel::AuxillaryTraitsTypes;
		pump->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Pump);
		pump->AuxillaryTraits.Set(LabelTrait{}, LabelTrait::COMMON_LABEL_FILTER_PUMP);
		pump->AuxillaryTraits.Set(PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);
		pump->AuxillaryTraits.Set(PumpStatusTrait{}, Kernel::PumpStatuses::Running);
	}
	data_hub->Devices.Add(pump);

	// Pump running but NO reading ever received -> not raised (*IsStale() is pure
	// age and false for a never-set reading; total silence is serial_comms_loss).
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::TemperatureStale));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::TemperatureStale), 0u);

	// A pool reading exists and the threshold is negative, so it is instantly
	// "older than the threshold" without the test having to sleep.
	data_hub->PoolTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(27.0));
	data_hub->TemperatureStalenessThreshold = std::chrono::seconds(-1);
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::TemperatureStale));
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::TemperatureStale), 1u);
	BOOST_CHECK_EQUAL("pool", rec.transitions.back().params.at("body").get<std::string>());

	// Pump stops -> the same staleness becomes the EXPECTED flavour -> clears.
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Off);
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::TemperatureStale));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::TemperatureStale), 2u);

	// Pump resumes with a sane threshold and the reading fresh again -> stays clear.
	pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Running);
	data_hub->TemperatureStalenessThreshold = std::chrono::seconds(600);
	data_hub->PoolTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(27.5));
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::TemperatureStale));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::TemperatureStale), 2u);   // no new transition
}

// In spa mode the ACTIVE body is the spa: a stale spa reading raises (body=spa)
// even though the pool reading is absent, and a stale POOL reading is ignored.
BOOST_AUTO_TEST_CASE(TemperatureStale_FollowsActiveBody)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();

	auto pump = std::make_shared<Kernel::AuxillaryDevice>();
	{
		using namespace Kernel::AuxillaryTraitsTypes;
		pump->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Pump);
		pump->AuxillaryTraits.Set(LabelTrait{}, LabelTrait::COMMON_LABEL_FILTER_PUMP);
		pump->AuxillaryTraits.Set(PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);
		pump->AuxillaryTraits.Set(PumpStatusTrait{}, Kernel::PumpStatuses::Running);
	}
	data_hub->Devices.Add(pump);

	data_hub->CirculationMode = Kernel::CirculationModes::Spa;
	data_hub->TemperatureStalenessThreshold = std::chrono::seconds(-1);

	// Only the POOL reading exists (and is stale) -> ignored while the spa is active.
	data_hub->PoolTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(27.0));
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::TemperatureStale));

	// A (stale) SPA reading appears -> raises, naming the spa.
	data_hub->SpaTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(36.0));
	monitor.EvaluateTemperatureStale();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::TemperatureStale));
	BOOST_CHECK_EQUAL("spa", rec.transitions.back().params.at("body").get<std::string>());
}

// While already raised and still below threshold, re-evaluating produces no new
// transition (the raise branch is guarded by !currently_raised) — latching holds.
BOOST_AUTO_TEST_CASE(SaltLow_StaysRaisedWhileStillBelow_NoNewTransition)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;
	settings.salt_low_ppm = 2600;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::PreferencesHub>()->AlertSaltLowPpm = 2600;
	auto data_hub = Find<Kernel::DataHub>();

	data_hub->SaltLevel(2000 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 1u);

	// A second, still-low reading: already raised, so neither branch fires again.
	data_hub->SaltLevel(1900 * Units::ppm);
	monitor.EvaluateSaltLow();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::SaltLow));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SaltLow), 1u);   // no new edge
}

// No chlorinator present clears (and never raises) both the warning and the fault
// conditions — the empty()-guard clear branch of each evaluator.
BOOST_AUTO_TEST_CASE(Chlorinator_NoDevicePresent_ClearsWarningAndFault)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	// No chlorinator on the DataHub at all: both evaluators take the "no chlorinator
	// present" clear path and never latch.
	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	// Starting from the cleared baseline, no transition is emitted for either.
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 0u);
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorFault), 0u);
}

// A chlorinator present but carrying NO health trait leaves both conditions clear:
// warning maps to nullopt (no label) and fault stays false (health absent).
BOOST_AUTO_TEST_CASE(Chlorinator_NoHealthTrait_StaysClear)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	using namespace Kernel::AuxillaryTraitsTypes;
	auto chlor = std::make_shared<Kernel::AuxillaryDevice>();
	chlor->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
	chlor->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
	// Deliberately NO ChlorinatorHealthTrait -> TryGet returns nullopt.
	Find<Kernel::DataHub>()->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));
}

// A health value that is neither a warning nor a hard fault (e.g. TurningOff)
// leaves both conditions clear: ChlorinatorWarningLabel returns nullopt and
// IsChlorinatorFault is false.
BOOST_AUTO_TEST_CASE(Chlorinator_NonWarningNonFaultHealth_StaysClear)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinator(Kernel::ChlorinatorHealth::TurningOff);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorFault));
}

// EvaluateAll fans out to every individual evaluator in one call — driving it with
// a Service-mode DataHub proves the aggregate dispatch reaches service_mode (and
// the other evaluators run without throwing on the shared hub).
BOOST_AUTO_TEST_CASE(EvaluateAll_DispatchesToEveryEvaluator)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::DataHub>()->Mode = Kernel::EquipmentMode::Service;

	BOOST_CHECK_NO_THROW(monitor.EvaluateAll());
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ServiceMode));
}

// serial_comms_loss does nothing before a baseline is established: with the
// monitor never Started, m_HaveCommsBaseline is false and the evaluator returns.
BOOST_AUTO_TEST_CASE(SerialCommsLoss_NoBaseline_IsNoOp)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	std::int64_t now = 1000;
	monitor.SetClock([&now] { return now; });

	// Never Started -> no comms baseline -> the evaluator takes its early return.
	now = 1'000'000;
	monitor.EvaluateSerialCommsLoss();
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SerialCommsLoss));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::SerialCommsLoss), 0u);
}

// BuildStateJson reports every catalogue condition as a "true"/"false" string
// matching the latched state (read by the HA binary_sensors).
BOOST_AUTO_TEST_CASE(BuildStateJson_ReflectsLatchedState)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);

	Find<Kernel::DataHub>()->Mode = Kernel::EquipmentMode::Service;
	monitor.EvaluateServiceMode();

	auto state = monitor.BuildStateJson();
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::ServiceMode }], "true");
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::SaltLow }], "false");
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::ChlorinatorFault }], "false");
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::ChlorinatorWarning }], "false");
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::SerialCommsLoss }], "false");
	BOOST_CHECK_EQUAL(state[std::string{ ConditionKeys::TemperatureStale }], "false");
}

// AddSink ignores a null sink: the guard drops it, so a subsequent transition is
// delivered only to the real sinks (no crash from invoking an empty std::function).
BOOST_AUTO_TEST_CASE(AddSink_NullSink_IsIgnored)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);

	// A default-constructed (empty) Sink is dropped by the guard in AddSink.
	monitor.AddSink(AlertMonitor::Sink{});

	// A real sink added afterwards still receives transitions, proving the null one
	// was skipped rather than stored-and-invoked (which would have crashed).
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	Find<Kernel::DataHub>()->Mode = Kernel::EquipmentMode::Service;
	monitor.EvaluateServiceMode();

	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ServiceMode));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ServiceMode), 1u);
}

// IsRaised returns false for a condition key that is not in the latch map at all
// (an unknown key never seeded by the catalogue) — the map-miss branch.
BOOST_AUTO_TEST_CASE(IsRaised_UnknownKey_ReturnsFalse)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);

	BOOST_CHECK(!monitor.IsRaised("not_a_real_condition_key"));
}

// Start() wires the DataHub and EquipmentHub signals to EvaluateAll: firing either
// signal after Start() re-evaluates every condition (proven here by observing the
// service_mode edge that a fired signal produces).
BOOST_AUTO_TEST_CASE(Start_SubscribesHubSignals_EvaluateOnFire)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	SinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	std::int64_t now = 5000;
	monitor.SetClock([&now] { return now; });

	auto data_hub = Find<Kernel::DataHub>();
	auto equipment_hub = Find<Kernel::EquipmentHub>();

	monitor.Start();

	// Put the controller into Service mode, then fire the DataHub config signal; the
	// subscription lambda installed by Start() calls EvaluateAll(), raising service_mode.
	// The AlertMonitor lambda ignores the event payload, so a null shared_ptr is fine.
	data_hub->Mode = Kernel::EquipmentMode::Service;
	data_hub->ConfigUpdateSignal(nullptr);
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ServiceMode));
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ServiceMode), 1u);

	// Leaving Service mode and firing the EquipmentHub signal re-evaluates and clears it,
	// proving the second subscription lambda is likewise wired.
	data_hub->Mode = Kernel::EquipmentMode::Normal;
	equipment_hub->EquipmentStatusChangeSignal(nullptr);
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ServiceMode));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ServiceMode), 2u);

	monitor.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
