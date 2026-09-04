#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>

#include "alerting/alert_condition.h"
#include "alerting/alert_monitor.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "options/options_alerting_options.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Alerting;

//=============================================================================
// AlertMonitor arms the main suite does not reach:
//
//   * the wire status byte is a BITFIELD, so several flags can be active at
//     once - the detail text must list every one of them, separated, rather
//     than naming only the first (the multi-part join);
//   * the lifecycle guards: Stop() without Start(), a repeated Stop(), and the
//     cancelled comms timer's completion handler.
//=============================================================================

namespace
{

	struct BranchSinkRecorder
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

	// A chlorinator whose health FLAGS carry several simultaneously-active
	// states (exactly what the decoded status byte can report).
	std::shared_ptr<Kernel::AuxillaryDevice> MakeChlorinatorWithFlags(std::set<Kernel::ChlorinatorHealth> flags, Kernel::ChlorinatorHealth worst)
	{
		using namespace Kernel::AuxillaryTraitsTypes;

		auto chlor = std::make_shared<Kernel::AuxillaryDevice>();
		chlor->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
		chlor->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
		chlor->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
		chlor->AuxillaryTraits.Set(ChlorinatorHealthTrait{}, worst);
		chlor->AuxillaryTraits.Set(ChlorinatorHealthFlagsTrait{}, std::move(flags));
		return chlor;
	}

}
// unnamed namespace

BOOST_FIXTURE_TEST_SUITE(TestSuite_AlertMonitorBranches, Test::HubLocatorInjector)

//-----------------------------------------------------------------------------
// Multi-flag detail text
//-----------------------------------------------------------------------------

// Two warnings active at once must BOTH appear in the detail, comma-separated,
// and both must be listed in the params - naming only the first would hide a
// second actionable problem from the log line, the UI toast and MQTT.
BOOST_AUTO_TEST_CASE(AlertBranches_ChlorinatorWarning_NamesEveryActiveFlag)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	BranchSinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinatorWithFlags(
		{ Kernel::ChlorinatorHealth::Warning_LowSalt, Kernel::ChlorinatorHealth::Warning_HighCurrent },
		Kernel::ChlorinatorHealth::Warning_HighCurrent);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();

	BOOST_REQUIRE(monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorWarning), 1u);

	const auto& transition = rec.transitions.back();

	// Both labels present, and joined rather than concatenated.
	BOOST_CHECK(transition.detail.find("Low salt") != std::string::npos);
	BOOST_CHECK(transition.detail.find("High current") != std::string::npos);
	BOOST_CHECK(transition.detail.find(", ") != std::string::npos);

	// Every matched flag is carried through in the params (the UI translates
	// these enum names itself), with `health` naming the first.
	BOOST_REQUIRE(transition.params.contains("health_flags"));
	BOOST_CHECK_EQUAL(transition.params.at("health_flags").size(), 2u);
	BOOST_CHECK_EQUAL("Warning_LowSalt", transition.params.at("health").get<std::string>());
}

// The same applies to hard faults: a cell reporting both fault states names both.
BOOST_AUTO_TEST_CASE(AlertBranches_ChlorinatorFault_NamesEveryActiveFlag)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	BranchSinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinatorWithFlags(
		{ Kernel::ChlorinatorHealth::Error_CheckPCB, Kernel::ChlorinatorHealth::GeneralFault },
		Kernel::ChlorinatorHealth::GeneralFault);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorFault();
	monitor.EvaluateChlorinatorWarning();

	BOOST_REQUIRE(monitor.IsRaised(ConditionKeys::ChlorinatorFault));
	// Hard faults are not warnings, however many of them there are.
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::ChlorinatorWarning));

	BOOST_REQUIRE_EQUAL(rec.CountFor(ConditionKeys::ChlorinatorFault), 1u);

	AlertTransition fault_transition{};
	for (const auto& t : rec.transitions)
	{
		if (t.condition == ConditionKeys::ChlorinatorFault) { fault_transition = t; }
	}

	BOOST_CHECK(fault_transition.detail.find("Check PCB") != std::string::npos);
	BOOST_CHECK(fault_transition.detail.find("General fault") != std::string::npos);
	BOOST_CHECK(fault_transition.detail.find(", ") != std::string::npos);
	BOOST_REQUIRE(fault_transition.params.contains("health_flags"));
	BOOST_CHECK_EQUAL(fault_transition.params.at("health_flags").size(), 2u);
}

// A fault and a warning reported together are surfaced as SEPARATE conditions,
// each naming only its own flags.
BOOST_AUTO_TEST_CASE(AlertBranches_MixedFlagsSplitAcrossConditions)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	BranchSinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	auto data_hub = Find<Kernel::DataHub>();
	auto chlor = MakeChlorinatorWithFlags(
		{ Kernel::ChlorinatorHealth::Warning_CleanCell, Kernel::ChlorinatorHealth::GeneralFault },
		Kernel::ChlorinatorHealth::GeneralFault);
	data_hub->Devices.Add(chlor);

	monitor.EvaluateChlorinatorWarning();
	monitor.EvaluateChlorinatorFault();

	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorWarning));
	BOOST_CHECK(monitor.IsRaised(ConditionKeys::ChlorinatorFault));

	for (const auto& t : rec.transitions)
	{
		if (t.condition == ConditionKeys::ChlorinatorWarning)
		{
			BOOST_CHECK(t.detail.find("Clean cell") != std::string::npos);
			BOOST_CHECK(t.detail.find("General fault") == std::string::npos);
			BOOST_CHECK_EQUAL(t.params.at("health_flags").size(), 1u);
		}
		else if (t.condition == ConditionKeys::ChlorinatorFault)
		{
			BOOST_CHECK(t.detail.find("General fault") != std::string::npos);
			BOOST_CHECK(t.detail.find("Clean cell") == std::string::npos);
			BOOST_CHECK_EQUAL(t.params.at("health_flags").size(), 1u);
		}
	}
}

//-----------------------------------------------------------------------------
// Lifecycle guards
//-----------------------------------------------------------------------------

// Stop() on a monitor that was never started (and a second Stop()) must be
// inert: shutdown ordering is not guaranteed, so this is a real call pattern.
BOOST_AUTO_TEST_CASE(AlertBranches_StopWithoutStartIsInert)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	BranchSinkRecorder rec;
	monitor.AddSink(rec.AsSink());

	BOOST_CHECK_NO_THROW(monitor.Stop());

	monitor.Start();
	BOOST_CHECK_NO_THROW(monitor.Stop());
	BOOST_CHECK_NO_THROW(monitor.Stop());

	// Nothing was evaluated by the stop path itself.
	BOOST_CHECK(rec.transitions.empty());
}

// Stop() cancels the comms timer; its completion handler then runs with
// operation_aborted and must not re-arm the timer or evaluate anything.
BOOST_AUTO_TEST_CASE(AlertBranches_CancelledCommsTimerDoesNothing)
{
	boost::asio::io_context io;
	Options::Alerting::AlertingSettings settings;

	AlertMonitor monitor(io, *this, settings);
	BranchSinkRecorder rec;

	std::int64_t now = 1'000;
	monitor.SetClock([&now] { return now; });
	monitor.Start();
	monitor.AddSink(rec.AsSink());
	monitor.Stop();

	// Drain the cancelled timer completion.
	BOOST_CHECK_NO_THROW(io.poll());

	BOOST_CHECK(rec.transitions.empty());
	BOOST_CHECK(!monitor.IsRaised(ConditionKeys::SerialCommsLoss));
}

BOOST_AUTO_TEST_SUITE_END()
