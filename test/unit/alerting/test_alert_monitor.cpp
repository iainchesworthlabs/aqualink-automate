#include <chrono>
#include <cstdint>
#include <memory>
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

	std::shared_ptr<Kernel::AuxillaryDevice> MakeChlorinator(Kernel::ChlorinatorHealth health)
	{
		using namespace Kernel::AuxillaryTraitsTypes;
		auto chlor = std::make_shared<Kernel::AuxillaryDevice>();
		chlor->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlor->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlor->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		chlor->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, health);
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

	chlor->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::GeneralFault);
	monitor.EvaluateChlorinatorFault();
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	BOOST_CHECK_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorFault), 1u);

	chlor->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Ok);
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
	chlor->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Warning_NoFlow);
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
	chlor->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::Ok);
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

BOOST_AUTO_TEST_SUITE_END()
