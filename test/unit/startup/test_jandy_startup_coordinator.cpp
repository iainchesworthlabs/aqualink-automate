#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/startup/jandy_startup_coordinator.h"

using namespace AqualinkAutomate::Jandy::Startup;
using DeviceType = AqualinkAutomate::Devices::JandyEmulatedDeviceTypes;

namespace
{
	struct EmulatedRecord
	{
		DeviceType type;
		std::uint8_t id;
		std::string role;
	};

	// Mock environment: configurable probe/occupancy/identity; records EmulateDevice calls so
	// the whole start-up flow is exercised without hubs or hardware.
	struct MockEnvironment : public IStartupEnvironment
	{
		std::set<std::uint8_t> probes;
		std::set<std::uint8_t> occupied;
		std::string model;
		std::string revision;
		std::vector<EmulatedRecord> emulated;

		void EmulateDevice(DeviceType type, std::uint8_t id, std::string_view role) override
		{
			emulated.push_back(EmulatedRecord{ type, id, std::string(role) });
		}
		std::set<std::uint8_t> ObservedProbes() const override { return probes; }
		std::set<std::uint8_t> OccupiedAddresses() const override { return occupied; }
		std::string PanelModel() const override { return model; }
		std::string PanelRevision() const override { return revision; }

		const EmulatedRecord* Find(DeviceType type) const
		{
			for (const auto& record : emulated)
			{
				if (record.type == type)
				{
					return &record;
				}
			}
			return nullptr;
		}
	};
}

BOOST_AUTO_TEST_SUITE(Jandy_Startup_Coordinator_TestSuite)

BOOST_AUTO_TEST_CASE(Begin_StandsUpSerialAdapterAndEntersDetecting)
{
	MockEnvironment env;
	StartupCoordinator coord(env);

	coord.Begin();

	BOOST_CHECK(coord.CurrentPhase() == StartupCoordinator::Phase::Detecting);
	BOOST_REQUIRE(env.Find(DeviceType::SerialAdapter) != nullptr);
	BOOST_CHECK_EQUAL(env.Find(DeviceType::SerialAdapter)->id, 0x48);
}

BOOST_AUTO_TEST_CASE(Detecting_HoldsUntilControllerClassified)
{
	MockEnvironment env;  // no controller probes yet
	StartupCoordinator coord(env);
	coord.Begin();

	BOOST_CHECK(coord.Advance(/*detection_window_elapsed=*/false) == StartupCoordinator::Phase::Detecting);
	BOOST_CHECK(env.Find(DeviceType::IAQ) == nullptr);
	BOOST_CHECK(env.Find(DeviceType::OneTouch) == nullptr);
}

BOOST_AUTO_TEST_CASE(AqualinkTouchPanel_FreeBus_EmulatesIAQAt0x33_PagePush)
{
	MockEnvironment env;
	env.probes = { 0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x48, 0xA3 };  // PD-8 Combo-like probe set
	StartupCoordinator coord(env);

	coord.Begin();
	coord.Advance(false);  // classifiable from probes -> runs through Engaging to Running

	BOOST_CHECK(coord.CurrentPhase() == StartupCoordinator::Phase::Running);
	BOOST_CHECK(coord.Plan().method == DataGatheringMethod::PagePush);
	BOOST_REQUIRE(env.Find(DeviceType::IAQ) != nullptr);
	BOOST_CHECK_EQUAL(env.Find(DeviceType::IAQ)->id, 0x33);
	BOOST_CHECK(env.Find(DeviceType::OneTouch) == nullptr);  // page-push chosen, not spider
}

BOOST_AUTO_TEST_CASE(RealTouchAt0x33_RunsEmulatedAtSecondSlot_0x32)
{
	// The requested scenario: confirm 0x33 isn't already present; it IS -> run a second one.
	MockEnvironment env;
	env.probes = { 0x30, 0x31, 0x32, 0x33 };
	env.occupied = { 0x33 };  // a real AqualinkTouch answers at 0x33
	StartupCoordinator coord(env);

	coord.Begin();
	coord.Advance(false);

	BOOST_REQUIRE(env.Find(DeviceType::IAQ) != nullptr);
	BOOST_CHECK_EQUAL(env.Find(DeviceType::IAQ)->id, 0x32);
}

BOOST_AUTO_TEST_CASE(OneTouchOnlyPanel_FallsBackToSpider)
{
	MockEnvironment env;
	env.probes = { 0x40, 0x41, 0x42, 0x43, 0x48 };  // no AqualinkTouch range probed
	StartupCoordinator coord(env);

	coord.Begin();
	coord.Advance(false);

	BOOST_CHECK(coord.Plan().method == DataGatheringMethod::MenuSpider);
	BOOST_REQUIRE(env.Find(DeviceType::OneTouch) != nullptr);
	BOOST_CHECK_EQUAL(env.Find(DeviceType::OneTouch)->id, 0x41);
	BOOST_CHECK(env.Find(DeviceType::IAQ) == nullptr);
}

BOOST_AUTO_TEST_CASE(WindowElapsesWithNoController_ObserveOnly_NoControllerStoodUp)
{
	MockEnvironment env;  // no controller probes ever observed
	StartupCoordinator coord(env);

	coord.Begin();
	auto phase = coord.Advance(/*detection_window_elapsed=*/true);

	BOOST_CHECK(phase == StartupCoordinator::Phase::Running);
	BOOST_CHECK(coord.Plan().method == DataGatheringMethod::ObserveOnly);
	BOOST_CHECK(env.Find(DeviceType::IAQ) == nullptr);
	BOOST_CHECK(env.Find(DeviceType::OneTouch) == nullptr);
	BOOST_CHECK(env.Find(DeviceType::SerialAdapter) != nullptr);  // still up from Begin()
}

BOOST_AUTO_TEST_CASE(RevisionTouchCapable_ClassifiesEarlyWithoutWaitingForProbes)
{
	// The SerialAdapter sources "REV T.0.1" before the master's probe cycle is observed; a
	// touch-capable revision (Rev Q+) lets the coordinator classify and stand up the IAQ
	// emulation immediately, without waiting out the detection window.
	MockEnvironment env;
	env.revision = "REV T.0.1";  // no probes yet
	StartupCoordinator coord(env);

	coord.Begin();
	coord.Advance(/*detection_window_elapsed=*/false);

	BOOST_CHECK(coord.CurrentPhase() == StartupCoordinator::Phase::Running);
	BOOST_CHECK(coord.Plan().method == DataGatheringMethod::PagePush);
	BOOST_REQUIRE(env.Find(DeviceType::IAQ) != nullptr);
	BOOST_CHECK_EQUAL(env.Find(DeviceType::IAQ)->id, 0x33);
}

BOOST_AUTO_TEST_CASE(Advance_BeforeBegin_StaysIdle)
{
    // Advancing a freshly-constructed coordinator (Begin() never called) must sit in the Idle
    // arm and stand nothing up -- there is no detection in progress to react to.
    MockEnvironment env;
    StartupCoordinator coord(env);

    BOOST_CHECK(coord.CurrentPhase() == StartupCoordinator::Phase::Idle);

    const auto phase = coord.Advance(/*detection_window_elapsed=*/true);

    BOOST_CHECK(phase == StartupCoordinator::Phase::Idle);
    BOOST_CHECK(env.emulated.empty());   // Idle does nothing
}

BOOST_AUTO_TEST_CASE(Advance_AfterRunning_IsIdempotent)
{
    // Once Running, a further Advance() takes the Running arm (a no-op) and stands up no
    // additional emulation beyond what Engaging already placed.
    MockEnvironment env;
    env.probes = { 0x40, 0x41, 0x42, 0x43, 0x48 };  // OneTouch panel -> spider
    StartupCoordinator coord(env);

    coord.Begin();
    coord.Advance(false);
    BOOST_REQUIRE(coord.CurrentPhase() == StartupCoordinator::Phase::Running);

    const auto emulated_after_first_run = env.emulated.size();
    const auto phase = coord.Advance(false);   // Running arm -> no further work

    BOOST_CHECK(phase == StartupCoordinator::Phase::Running);
    BOOST_CHECK_EQUAL(env.emulated.size(), emulated_after_first_run);
}

BOOST_AUTO_TEST_CASE(Engaging_UnresolvedController_LogsAndSkips_NoEmulation)
{
    // A OneTouch panel where every usable OneTouch instance is occupied by a real device:
    // units 1-3 (0x40, 0x41, 0x42) are taken and unit 4 (0x43) is the reserved slot the
    // planner never offers. ResolveAddresses leaves the OneTouch UNRESOLVED, so Engaging takes
    // the "no free bus address -> warn + skip" arm and never emulates a controller.
    MockEnvironment env;
    env.probes = { 0x40, 0x41, 0x42 };
    env.occupied = { 0x40, 0x41, 0x42 };
    StartupCoordinator coord(env);

    coord.Begin();
    const auto phase = coord.Advance(false);

    BOOST_CHECK(phase == StartupCoordinator::Phase::Running);
    BOOST_CHECK(coord.Plan().method == DataGatheringMethod::MenuSpider);
    // The controller emulation was skipped (unresolved); only the SerialAdapter from Begin() ran.
    BOOST_CHECK(env.Find(DeviceType::OneTouch) == nullptr);
    BOOST_CHECK(env.Find(DeviceType::SerialAdapter) != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
