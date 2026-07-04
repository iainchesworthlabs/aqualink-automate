#include <cstdint>

#include <boost/asio/io_context.hpp>
#include <boost/test/unit_test.hpp>

#include "jandy/devices/jandy_device_types.h"
#include "jandy/startup/jandy_startup_service.h"
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "interfaces/idevice.h"

#include "support/unit_test_mockreplayharness.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Jandy::Startup;

// The JandyStartupService is the io_context glue around the StartupCoordinator: Start() begins the
// coordinator and arms the tick timer; each Tick() advances the coordinator and either stops (on
// reaching Running) or re-arms the 1 s timer; Stop() cancels the timer and halts ticking. These
// tests drive it against the real hubs (via the replay harness) with an explicitly-run io_context,
// deterministically, without hardware.

BOOST_AUTO_TEST_SUITE(Jandy_Startup_Service_TestSuite)

BOOST_AUTO_TEST_CASE(Start_ClassifiesImmediately_StandsUpTheControllerAndStopsTicking)
{
	// A touch-capable panel revision is available up-front (sourced by the SerialAdapter the
	// coordinator stands up in Begin()), so the coordinator classifies on the very first Advance()
	// without waiting out the detection window -- the service reaches steady state and does NOT
	// re-arm its timer, so running the io_context returns promptly with no pending work.
	Test::MockReplayHarness harness;
	harness.DataHub()->EquipmentVersions.Set("Revision", "REV T.0.1");

	boost::asio::io_context io_context;
	JandyStartupService service(io_context, harness.HubLocatorRef());

	service.Start();

	// An emulated AqualinkTouch (IAQ) is stood up at 0x33 for a touch-capable panel.
	BOOST_CHECK(harness.EquipmentHub()->FindDevice([](const Interfaces::IDevice& device)
	{
		const auto* jandy_type = dynamic_cast<const Devices::JandyDeviceType*>(&device.DeviceId());
		return (jandy_type != nullptr) && (jandy_type->Id()() == 0x33);
	}) != nullptr);

	// No further work was queued (the coordinator reached Running and stopped ticking).
	const auto handlers_run = io_context.run();
	BOOST_CHECK_EQUAL(handlers_run, 0U);
}

BOOST_AUTO_TEST_CASE(Start_NoController_ArmsTheTickTimer)
{
	// With no controller classifiable yet the coordinator stays Detecting, so the service arms its
	// 1 s tick timer. The pending timer is observable as queued io_context work (poll returns 0
	// handlers ready right now -- the timer has not yet expired -- but the context is not stopped).
	Test::MockReplayHarness harness;

	boost::asio::io_context io_context;
	JandyStartupService service(io_context, harness.HubLocatorRef());

	service.Start();

	// The tick timer is pending (not yet expired) -> nothing ready to run immediately.
	BOOST_CHECK_EQUAL(io_context.poll(), 0U);
	BOOST_CHECK(!io_context.stopped());
}

BOOST_AUTO_TEST_CASE(Stop_CancelsThePendingTick_NoHandlersRun)
{
	// Stop() sets the stopped flag and cancels the armed timer, so the outstanding async_wait
	// completes with operation_aborted and Tick() is not re-entered -- running the io_context
	// drains the (aborted) wait and then has no further work.
	Test::MockReplayHarness harness;

	boost::asio::io_context io_context;
	JandyStartupService service(io_context, harness.HubLocatorRef());

	service.Start();       // arms the tick timer (no controller -> Detecting)
	service.Stop();        // cancels it

	// The cancelled wait's completion runs (with an error code) but Tick() short-circuits on the
	// stopped flag, so no new timer is armed and the context finishes.
	io_context.run();
	BOOST_CHECK(io_context.stopped());
}

BOOST_AUTO_TEST_SUITE_END()
