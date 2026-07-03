#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/iaq_device.h"
#include "jandy/devices/iaq/iaq_page_registry.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"

#include "kernel/data_hub.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water_ids.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

namespace IaqTraits = AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

namespace
{
	struct IAQDeviceFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		IAQDeviceFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x33)))
		{
		}

		std::shared_ptr<JandyDeviceType> device_type;
	};

	// Exposes the protected watchdog/update hooks so the start-up state machine can be
	// driven directly. The IAQDevice ctor arms its watchdog on the real clock, so the
	// tests invoke WatchdogTimeoutOccurred() rather than waiting on wall-clock time.
	struct TestIAQDevice : public IAQDevice
	{
		using IAQDevice::IAQDevice;
		void TriggerWatchdogTimeout() { WatchdogTimeoutOccurred(); }
		void SimulateAddressedTraffic() { ProcessControllerUpdates(); }
	};
}

BOOST_FIXTURE_TEST_SUITE(IAQDevice_TestSuite, IAQDeviceFixture)

// =============================================================================
// Construction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_Emulated)
{
	BOOST_CHECK_NO_THROW(IAQDevice device(device_type, *this, true));
}

BOOST_AUTO_TEST_CASE(TestConstruction_NonEmulated)
{
	BOOST_CHECK_NO_THROW(IAQDevice device(device_type, *this, false));
}

// =============================================================================
// QueueCommand
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueCommand_DoesNotThrow)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x19));
}

BOOST_AUTO_TEST_CASE(TestQueueCommand_MultipleCommands)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x19));
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x02));
	BOOST_CHECK_NO_THROW(device.QueueCommand(0x01));
}

// =============================================================================
// QueueChlorinatorPercentage
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueChlorinatorPercentage_DoesNotThrow)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorPercentage(75));
}

BOOST_AUTO_TEST_CASE(TestQueueChlorinatorPercentage_Zero)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorPercentage(0));
}

BOOST_AUTO_TEST_CASE(TestQueueChlorinatorPercentage_Hundred)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorPercentage(100));
}

// =============================================================================
// QueueChlorinatorBoost
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueueChlorinatorBoost_Enable)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorBoost(true));
}

BOOST_AUTO_TEST_CASE(TestQueueChlorinatorBoost_Disable)
{
	IAQDevice device(device_type, *this, true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorBoost(false));
}

// =============================================================================
// Command sequencing
// =============================================================================

BOOST_AUTO_TEST_CASE(TestQueuePercentage_ThenBoost_DoesNotThrow)
{
	IAQDevice device(device_type, *this, true);
	device.QueueChlorinatorPercentage(50);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorBoost(true));
}

BOOST_AUTO_TEST_CASE(TestQueueBoost_ThenPercentage_DoesNotThrow)
{
	IAQDevice device(device_type, *this, true);
	device.QueueChlorinatorBoost(true);
	BOOST_CHECK_NO_THROW(device.QueueChlorinatorPercentage(75));
}

// =============================================================================
// Destruction after queuing
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDestruction_AfterQueuing)
{
	{
		IAQDevice device(device_type, *this, true);
		device.QueueCommand(0x19);
		device.QueueChlorinatorPercentage(50);
		device.QueueChlorinatorBoost(false);
	}
	// If we reach here without crash, destruction is clean
	BOOST_CHECK(true);
}

// =============================================================================
// Start-up watchdog states (regression for the live iAqualink2 capture)
// =============================================================================

BOOST_AUTO_TEST_CASE(TestWatchdog_NeverAddressedEmulated_BecomesNotPresentNotFault)
{
	// An emulated IAQ id the master never addresses (e.g. the default 0xa1 on a
	// panel with no iAqualink2 configured there) must settle to NotPresent, NOT
	// fault -- nothing went wrong, the id simply isn't on the bus.
	TestIAQDevice device(device_type, *this, /*is_emulated=*/true);
	BOOST_REQUIRE(!device.IsFaulted());
	BOOST_REQUIRE(!device.IsNotPresent());

	device.TriggerWatchdogTimeout();   // 30s elapsed with no traffic ever addressed

	BOOST_CHECK(device.IsNotPresent());
	BOOST_CHECK(!device.IsFaulted());
}

BOOST_AUTO_TEST_CASE(TestWatchdog_AddressedThenSilent_Faults)
{
	// A device that WAS receiving traffic addressed to its id and then went silent
	// is a genuine fault, distinct from "never present".
	TestIAQDevice device(device_type, *this, /*is_emulated=*/true);
	device.SimulateAddressedTraffic();   // traffic seen -> m_HasReceivedData = true
	device.TriggerWatchdogTimeout();     // ...then it stopped

	BOOST_CHECK(device.IsFaulted());
	BOOST_CHECK(!device.IsNotPresent());
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// System Status screen rendering
//
// Regression for the "Actual Devices" diagnostics card showing the IAQ as
// "page unknown" with no content.  The IAQ (iAqualink2 cloud interface) has no
// navigable physical screen, so after decoding a MainStatus (0x70) it must
// render the live status into its Screen capability as a fixed System Status
// page -- DescribeDiagnostics()/DescribeScreen() must then report a KNOWN page
// type and lines that carry the decoded values.
// =============================================================================

namespace
{
	constexpr uint8_t IAQ_DEVICE_ID = 0x33;   // AqualinkTouch address carrying IAQ status.

	// Build a current-format (no-sentinel) MainStatus (0x70) payload matching the
	// wire layout exercised in test_iaq_message_main_status.cpp:
	//   device_count, device_ids..., pump, pool_heat, spa_mode, spa_heat, solar,
	//   pool_target(BE C), spa_target(BE C), air(BE C), water_current(BE C)
	std::vector<uint8_t> MakeMainStatusPayload_CurrentFormat()
	{
		auto push_temp_be = [](std::vector<uint8_t>& v, uint16_t raw)
		{
			v.push_back(static_cast<uint8_t>(raw >> 8));
			v.push_back(static_cast<uint8_t>(raw & 0xFF));
		};

		std::vector<uint8_t> payload;
		payload.push_back(0x03);       // device_count = 3
		payload.push_back(0x01);       // device IDs
		payload.push_back(0x02);
		payload.push_back(0x08);
		payload.push_back(0x01);       // pump ON
		payload.push_back(0x01);       // pool heater = Heating
		payload.push_back(0x00);       // spa OFF (Pool mode)
		payload.push_back(0x00);       // spa heater = Off
		payload.push_back(0x00);       // solar = Off
		push_temp_be(payload, 28);     // pool_target  = 28C (pool heat SETPOINT)
		push_temp_be(payload, 32);     // spa_target   = 32C (spa heat SETPOINT)
		push_temp_be(payload, 24);     // air          = 24C (== 75F)
		push_temp_be(payload, 27);     // water_current = 27C -> pool ACTUAL temp (pool mode, pump on; == 81F)
		return payload;
	}
}

BOOST_AUTO_TEST_SUITE(IAQDevice_StatusScreen_TestSuite)

BOOST_AUTO_TEST_CASE(MainStatus_RendersKnownSystemStatusPage)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	// IAQDevice ctor takes (device_id, hub_locator, is_emulated); the harness's
	// AddDevice<> appends the hub locator last, which would not match this middle-
	// positioned hub_locator parameter, so construct the device directly against
	// the harness's HubLocator and keep it alive for the replay.
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, MakeMainStatusPayload_CurrentFormat());
	harness.Replay(frame);

	auto diagnostics = device.DescribeDiagnostics();
	BOOST_REQUIRE(diagnostics.contains("screen"));

	const auto& screen = diagnostics["screen"];
	BOOST_REQUIRE(screen.contains("page_type"));
	BOOST_REQUIRE(screen.contains("lines"));

	// (a) The page type must no longer be the constructor-default Page_Unknown.
	const auto page_type = screen["page_type"].get<std::string>();
	BOOST_CHECK_NE(page_type, std::string("Page_Unknown"));
	BOOST_CHECK_EQUAL(page_type, std::string("Page_SystemStatus"));

	// (b) The rendered lines must carry the decoded live status.
	std::string joined;
	for (const auto& line : screen["lines"])
	{
		joined += line.get<std::string>();
		joined += '\n';
	}

	BOOST_CHECK(joined.find("System Status") != std::string::npos);
	BOOST_CHECK(joined.find("Pool Temp") != std::string::npos);
	BOOST_CHECK(joined.find("Pump: On") != std::string::npos);
	// Pool mode + pump on: water_current (27C == 81F) renders as the pool's actual
	// temperature and air (24C == 75F) renders; the spa has no reading, so it must
	// show "--" -- and the pool SETPOINT (28C == 82F) must NOT appear as a temp line.
	BOOST_CHECK(joined.find("Pool Temp: 81F") != std::string::npos);
	BOOST_CHECK(joined.find("Air Temp:  75F") != std::string::npos);
	BOOST_CHECK(joined.find("Spa Temp:  --") != std::string::npos);
	BOOST_CHECK(joined.find("Pool Temp: 82F") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(PageSurvey_OnHomeEstablished_QueuesNavigation)
{
	// An emulated AqualinkTouch with a page survey armed walks its data pages once the home
	// page is established (first MainStatus) -- targeted navigation, not a menu crawl. The
	// navigation drains one command per poll, so it shows up as a non-empty command queue.
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);
	device.EnablePageSurvey(IAQ::DefaultAqualinkTouchRegistry());

	// Nothing queued until home is established.
	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["command_queue_depth"].get<std::uint32_t>(), 0u);

	const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);
	harness.Replay(Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, MakeMainStatusPayload_CurrentFormat()));

	// Home established -> the survey navigation sequence is queued.
	BOOST_CHECK_GT(device.DescribeDiagnostics()["command_queue_depth"].get<std::uint32_t>(), 0u);
}

BOOST_AUTO_TEST_CASE(PageSurvey_NotEnabled_NothingQueuedOnHome)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);
	// Survey NOT armed -> reaching home must not queue any navigation.

	const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);
	harness.Replay(Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, MakeMainStatusPayload_CurrentFormat()));

	BOOST_CHECK_EQUAL(device.DescribeDiagnostics()["command_queue_depth"].get<std::uint32_t>(), 0u);
}

BOOST_AUTO_TEST_CASE(Probe_EmulatedAnswers_NonEmulatedIgnores)
{
	// The master discovers an AqualinkTouch (0x33) with a generic Probe (cmd 0x00),
	// exactly as it discovers a OneTouch. An EMULATED instance must answer it (treating
	// the probe as "addressed", so it is present rather than NotPresent on timeout) -- this
	// is what lets the PowerCenter sim go on to drive the IAQ page protocol. A passive
	// non-emulated decoder must IGNORE a bare probe (a probe alone is not proof that a real
	// device answered) and settle to NotPresent.
	const uint8_t cmd_probe = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::Probe);
	auto probe = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_probe, {});

	{
		Test::MockReplayHarness harness;
		auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
		TestIAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/true);
		harness.Replay(probe);
		device.TriggerWatchdogTimeout();
		BOOST_CHECK(device.IsFaulted());        // answered the probe -> was addressed
		BOOST_CHECK(!device.IsNotPresent());
	}
	{
		Test::MockReplayHarness harness;
		auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
		TestIAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);
		harness.Replay(probe);
		device.TriggerWatchdogTimeout();
		BOOST_CHECK(device.IsNotPresent());     // bare probe ignored -> not present
		BOOST_CHECK(!device.IsFaulted());
	}
}

BOOST_AUTO_TEST_CASE(MainStatus_ScreenRefreshesOnSecondMessage)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);

	// First MainStatus: pump ON.
	auto frame_on = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, MakeMainStatusPayload_CurrentFormat());
	harness.Replay(frame_on);

	// Second MainStatus: same layout but pump OFF (byte after the 3 device IDs).
	auto payload_off = MakeMainStatusPayload_CurrentFormat();
	payload_off[4] = 0x00;   // pump OFF
	auto frame_off = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, payload_off);
	harness.Replay(frame_off);

	auto diagnostics = device.DescribeDiagnostics();
	std::string joined;
	for (const auto& line : diagnostics["screen"]["lines"])
	{
		joined += line.get<std::string>();
		joined += '\n';
	}

	// The page must still be known and must reflect the LATEST state (pump off),
	// proving the render is idempotent/refreshing rather than accumulating stale lines.
	BOOST_CHECK_EQUAL(diagnostics["screen"]["page_type"].get<std::string>(), std::string("Page_SystemStatus"));
	BOOST_CHECK(joined.find("Pump: Off") != std::string::npos);
	BOOST_CHECK(joined.find("Pump: On") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// Cloud Link heartbeat screen rendering
//
// The heartbeat-only IAQ (the iAqualink2 cloud interface on 0xA3) receives ONLY
// the heartbeat (0x53) -- no MainStatus/AuxStatus and no navigable page.  Without
// a rendered page it would sit on the constructor-default Page_Unknown forever.
// On a heartbeat it must render a fixed "Cloud Link" page carrying the heartbeat
// liveness; a 0x33 that has decoded a MainStatus must KEEP its System Status page
// even when a heartbeat arrives (the two ids share one handler).
// =============================================================================

namespace
{
	constexpr uint8_t IAQ_CLOUD_DEVICE_ID = 0xA3;   // iAqualink2 cloud interface (heartbeat-only).
}

BOOST_AUTO_TEST_SUITE(IAQDevice_CloudLinkScreen_TestSuite)

BOOST_AUTO_TEST_CASE(Heartbeat_OnHeartbeatOnlyDevice_RendersCloudLinkPage)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_CLOUD_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	const uint8_t cmd_heartbeat = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_Heartbeat);
	auto frame = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_CLOUD_DEVICE_ID, cmd_heartbeat, /*payload=*/{});
	harness.Replay(frame);

	auto diagnostics = device.DescribeDiagnostics();
	BOOST_REQUIRE(diagnostics.contains("screen"));

	const auto& screen = diagnostics["screen"];
	BOOST_REQUIRE(screen.contains("page_type"));
	BOOST_REQUIRE(screen.contains("lines"));

	// (a) A heartbeat-only device that never saw a MainStatus renders Cloud Link,
	//     not the constructor-default Page_Unknown and not System Status.
	const auto page_type = screen["page_type"].get<std::string>();
	BOOST_CHECK_EQUAL(page_type, std::string("Page_CloudLink"));
	BOOST_CHECK_NE(page_type, std::string("Page_Unknown"));
	BOOST_CHECK_NE(page_type, std::string("Page_SystemStatus"));

	// (b) The rendered lines must mention the cloud link and the heartbeat liveness.
	std::string joined;
	for (const auto& line : screen["lines"])
	{
		joined += line.get<std::string>();
		joined += '\n';
	}

	BOOST_CHECK(joined.find("Cloud Link") != std::string::npos);
	BOOST_CHECK(joined.find("Heartbeat") != std::string::npos);
	// A just-Kick()ed watchdog reports the link as active.
	BOOST_CHECK(joined.find("active") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Heartbeat_AfterMainStatus_KeepsSystemStatusPage)
{
	Test::MockReplayHarness harness;

	// A device on the AqualinkTouch 0x33 side that HAS decoded a MainStatus must
	// keep its System Status page even after a later heartbeat arrives -- it must
	// NOT flip to Cloud Link just because a heartbeat was seen.
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);
	auto main_status_frame = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, MakeMainStatusPayload_CurrentFormat());
	harness.Replay(main_status_frame);

	// Sanity: System Status rendered from the MainStatus.
	BOOST_REQUIRE_EQUAL(device.DescribeDiagnostics()["screen"]["page_type"].get<std::string>(), std::string("Page_SystemStatus"));

	// Now a heartbeat addressed to the same id arrives.
	const uint8_t cmd_heartbeat = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_Heartbeat);
	auto heartbeat_frame = Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_heartbeat, /*payload=*/{});
	harness.Replay(heartbeat_frame);

	auto diagnostics = device.DescribeDiagnostics();
	const auto page_type = diagnostics["screen"]["page_type"].get<std::string>();

	// The page must STILL be System Status -- the heartbeat must not clobber it.
	BOOST_CHECK_EQUAL(page_type, std::string("Page_SystemStatus"));
	BOOST_CHECK_NE(page_type, std::string("Page_CloudLink"));

	std::string joined;
	for (const auto& line : diagnostics["screen"]["lines"])
	{
		joined += line.get<std::string>();
		joined += '\n';
	}
	BOOST_CHECK(joined.find("System Status") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// MainStatus -> DataHub state
//
// ProcessMainStatus does not just render the screen: it is the single authority
// that fans the decoded MainStatus out into the DataHub (circulation mode + active
// body, the three temperatures, the filter pump, and the Pool/Spa/Solar heaters).
// The screen-rendering tests above never inspect that DataHub state, so these
// drive the same proven MainStatus frame and assert the decoded model.
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQDevice_MainStatusDataHub_TestSuite)

namespace
{
	void ReplayMainStatus(Test::MockReplayHarness& harness, const std::vector<uint8_t>& payload)
	{
		const uint8_t cmd_main_status = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_MainStatus);
		harness.Replay(Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_main_status, payload));
	}
}

BOOST_AUTO_TEST_CASE(MainStatus_PopulatesDataHubTemperatures)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	ReplayMainStatus(harness, MakeMainStatusPayload_CurrentFormat());

	auto hub = harness.DataHub();
	auto pool = hub->PoolTemp();
	auto spa = hub->SpaTemp();
	auto air = hub->AirTemp();

	BOOST_REQUIRE(pool.has_value());
	BOOST_REQUIRE(air.has_value());

	// Pool mode + pump on: PoolTemp = water_current (27C actual), AirTemp = air (24C).
	// The spa is not circulating, so no spa reading may be written.
	BOOST_CHECK_CLOSE(pool.value().InCelsius().value(), 27.0, 0.01);
	BOOST_CHECK(!spa.has_value());
	BOOST_CHECK_CLOSE(air.value().InCelsius().value(), 24.0, 0.01);

	// Both heat setpoints are decoded from the targets on every message.
	auto pool_sp = hub->PoolTempSetpoint();
	auto spa_sp = hub->SpaTempSetpoint();
	BOOST_REQUIRE(pool_sp.has_value());
	BOOST_REQUIRE(spa_sp.has_value());
	BOOST_CHECK_CLOSE(pool_sp.value().InCelsius().value(), 28.0, 0.01);
	BOOST_CHECK_CLOSE(spa_sp.value().InCelsius().value(), 32.0, 0.01);
}

BOOST_AUTO_TEST_CASE(MainStatus_PumpOff_DoesNotFabricateOrOverwriteTemperatures)
{
	// Regression: with the pump off the controller stops measuring water temperature
	// (water_current carries a sentinel) but keeps sending MainStatus every second.
	// Previously each of those messages wrote the pool SETPOINT into PoolTemp, so the
	// UI showed e.g. "30C" (fresh, never stale) instead of flagging the reading as
	// stale.  A pump-off MainStatus must leave the last real reading (and its
	// timestamp) untouched.
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// Pump on: a real pool reading (27C) lands.
	ReplayMainStatus(harness, MakeMainStatusPayload_CurrentFormat());
	auto hub = harness.DataHub();
	BOOST_REQUIRE(hub->PoolTemp().has_value());
	const auto updated_at_before = hub->PoolTempUpdatedAt();
	BOOST_REQUIRE(updated_at_before.has_value());

	// Pump off: water_current degrades to the 0x0001 sentinel.
	auto payload_off = MakeMainStatusPayload_CurrentFormat();
	payload_off[4] = 0x00;         // pump OFF (byte after the 3 device IDs)
	payload_off[15] = 0x00;        // water_current high byte
	payload_off[16] = 0x01;        // water_current low byte = 0x0001 sentinel
	ReplayMainStatus(harness, payload_off);

	// The last real reading survives: still 27C (NOT the 28C setpoint) and its
	// timestamp was not refreshed, so staleness tracking can age it out.
	BOOST_REQUIRE(hub->PoolTemp().has_value());
	BOOST_CHECK_CLOSE(hub->PoolTemp().value().InCelsius().value(), 27.0, 0.01);
	BOOST_REQUIRE(hub->PoolTempUpdatedAt().has_value());
	BOOST_CHECK(hub->PoolTempUpdatedAt().value() == updated_at_before.value());
}

BOOST_AUTO_TEST_CASE(MainStatus_PoolMode_SetsCirculationPool)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// Default payload has spa_mode = 0x00 (Pool mode).
	ReplayMainStatus(harness, MakeMainStatusPayload_CurrentFormat());

	BOOST_CHECK(harness.DataHub()->CirculationMode == Kernel::CirculationModes::Pool);
}

BOOST_AUTO_TEST_CASE(MainStatus_SpaMode_SetsCirculationSpaAndAutoDetectsDualBody)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	auto payload = MakeMainStatusPayload_CurrentFormat();
	payload[6] = 0x01;   // spa_mode ON (the byte after pump + pool_heat)
	ReplayMainStatus(harness, payload);

	auto hub = harness.DataHub();
	BOOST_CHECK(hub->CirculationMode == Kernel::CirculationModes::Spa);
	// Seeing spa mode on an otherwise-unconfigured IAQ infers a shared-equipment dual body.
	BOOST_CHECK(hub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_SharedEquipment);
}

BOOST_AUTO_TEST_CASE(MainStatus_CreatesFilterPump_TrackingPumpStatus)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// First MainStatus: pump ON.
	ReplayMainStatus(harness, MakeMainStatusPayload_CurrentFormat());

	auto pumps = harness.DataHub()->FilterPumps();
	BOOST_REQUIRE_EQUAL(pumps.size(), 1u);
	auto status_on = pumps.front()->AuxillaryTraits.TryGet(IaqTraits::PumpStatusTrait{});
	BOOST_REQUIRE(status_on.has_value());
	BOOST_CHECK(status_on.value() == Kernel::PumpStatuses::Running);

	// Second MainStatus: pump OFF -> the SAME pump flips status (no duplicate created).
	auto payload_off = MakeMainStatusPayload_CurrentFormat();
	payload_off[4] = 0x00;   // pump OFF
	ReplayMainStatus(harness, payload_off);

	auto pumps_after = harness.DataHub()->FilterPumps();
	BOOST_REQUIRE_EQUAL(pumps_after.size(), 1u);
	auto status_off = pumps_after.front()->AuxillaryTraits.TryGet(IaqTraits::PumpStatusTrait{});
	BOOST_REQUIRE(status_off.has_value());
	BOOST_CHECK(status_off.value() == Kernel::PumpStatuses::Off);
}

BOOST_AUTO_TEST_CASE(MainStatus_CreatesHeaters_WithDecodedStatus)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// Payload: pool_heat = 0x01 (Heating), spa_heat = 0x00 (Off), solar = 0x00 (Off).
	ReplayMainStatus(harness, MakeMainStatusPayload_CurrentFormat());

	auto hub = harness.DataHub();

	auto pool_heat = hub->Devices.FindByLabel("Pool Heat");
	BOOST_REQUIRE_EQUAL(pool_heat.size(), 1u);
	auto pool_status = pool_heat.front()->AuxillaryTraits.TryGet(IaqTraits::HeaterStatusTrait{});
	BOOST_REQUIRE(pool_status.has_value());
	BOOST_CHECK(pool_status.value() == Kernel::HeaterStatuses::Heating);

	auto spa_heat = hub->Devices.FindByLabel("Spa Heat");
	BOOST_REQUIRE_EQUAL(spa_heat.size(), 1u);
	auto spa_status = spa_heat.front()->AuxillaryTraits.TryGet(IaqTraits::HeaterStatusTrait{});
	BOOST_REQUIRE(spa_status.has_value());
	BOOST_CHECK(spa_status.value() == Kernel::HeaterStatuses::Off);
}

BOOST_AUTO_TEST_SUITE_END()

// =============================================================================
// AuxStatus -> DataHub auxillary devices
//
// ProcessAuxStatus decodes the IAQ AuxStatus (0x72) device list into the DataHub:
// each entry creates/reconciles an auxillary device keyed by its stable aux id,
// sets its on/off status, adopts the panel-provided label, and infers a body of
// water from the label ("Spa"/"Pool"). The existing suite never exercises this.
// Wire layout (see test_iaq_messages.cpp): num_devices, indices[], then per
// device: status(1) type(1) pad(2) name_len(1) name[].
// =============================================================================

BOOST_AUTO_TEST_SUITE(IAQDevice_AuxStatusDataHub_TestSuite)

namespace
{
	// Build one AuxStatus device entry (header + name) appended to `out`.
	void AppendAuxDevice(std::vector<uint8_t>& out, bool is_on, uint8_t type, const std::string& name)
	{
		out.push_back(is_on ? 0x01 : 0x00);
		out.push_back(type);
		out.push_back(0x00);
		out.push_back(0x00);
		out.push_back(static_cast<uint8_t>(name.size()));
		out.insert(out.end(), name.begin(), name.end());
	}

	void ReplayAuxStatus(Test::MockReplayHarness& harness, const std::vector<uint8_t>& payload)
	{
		const uint8_t cmd_aux = static_cast<uint8_t>(AqualinkAutomate::Messages::JandyMessageIds::IAQ_AuxStatus);
		harness.Replay(Test::MessageBuilder::CreateValidChecksummedMessage(IAQ_DEVICE_ID, cmd_aux, payload));
	}
}

BOOST_AUTO_TEST_CASE(AuxStatus_CreatesLabelledAuxWithPoolBody)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// One device: aux index 5, ON, label "Pool Light".
	std::vector<uint8_t> payload = { 0x01, 0x05 };
	AppendAuxDevice(payload, /*is_on=*/true, /*type=*/0x00, "Pool Light");
	ReplayAuxStatus(harness, payload);

	auto matches = harness.DataHub()->Devices.FindByLabel("Pool Light");
	BOOST_REQUIRE_EQUAL(matches.size(), 1u);
	auto aux = matches.front();

	auto status = aux->AuxillaryTraits.TryGet(IaqTraits::AuxillaryStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::AuxillaryStatuses::On);

	// Label contains "Pool" -> body heuristic resolves to Pool.
	auto body = aux->AuxillaryTraits.TryGet(IaqTraits::BodyOfWaterTrait{});
	BOOST_REQUIRE(body.has_value());
	BOOST_CHECK(body.value() == Kernel::BodyOfWaterIds::Pool);
}

BOOST_AUTO_TEST_CASE(AuxStatus_SpaLabel_ResolvesSpaBody)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	std::vector<uint8_t> payload = { 0x01, 0x05 };
	AppendAuxDevice(payload, /*is_on=*/false, /*type=*/0x00, "Spa Jet");
	ReplayAuxStatus(harness, payload);

	auto matches = harness.DataHub()->Devices.FindByLabel("Spa Jet");
	BOOST_REQUIRE_EQUAL(matches.size(), 1u);

	auto body = matches.front()->AuxillaryTraits.TryGet(IaqTraits::BodyOfWaterTrait{});
	BOOST_REQUIRE(body.has_value());
	BOOST_CHECK(body.value() == Kernel::BodyOfWaterIds::Spa);

	auto status = matches.front()->AuxillaryTraits.TryGet(IaqTraits::AuxillaryStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::AuxillaryStatuses::Off);
}

BOOST_AUTO_TEST_CASE(AuxStatus_SecondMessage_UpdatesStatusInPlace)
{
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(IAQ_DEVICE_ID));
	IAQDevice device(device_id, harness.HubLocatorRef(), /*is_emulated=*/false);

	// First: aux 5 ON.
	std::vector<uint8_t> on = { 0x01, 0x05 };
	AppendAuxDevice(on, /*is_on=*/true, /*type=*/0x00, "Pool Light");
	ReplayAuxStatus(harness, on);

	// Second: same aux 5 OFF -> reconciled by stable id, no duplicate.
	std::vector<uint8_t> off = { 0x01, 0x05 };
	AppendAuxDevice(off, /*is_on=*/false, /*type=*/0x00, "Pool Light");
	ReplayAuxStatus(harness, off);

	auto matches = harness.DataHub()->Devices.FindByLabel("Pool Light");
	BOOST_REQUIRE_EQUAL(matches.size(), 1u);
	auto status = matches.front()->AuxillaryTraits.TryGet(IaqTraits::AuxillaryStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::AuxillaryStatuses::Off);
}

BOOST_AUTO_TEST_SUITE_END()
