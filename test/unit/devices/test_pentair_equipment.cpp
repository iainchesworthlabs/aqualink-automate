#include <cstdint>
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"

#include "pentair/devices/pentair_chlorinator_device.h"
#include "pentair/devices/pentair_controller_device.h"
#include "pentair/devices/pentair_device_id.h"
#include "pentair/devices/pentair_vsp_pump_device.h"
#include "pentair/equipment/pentair_equipment.h"
#include "pentair/messages/chlorinator/pentair_chlorinator_message_status.h"
#include "pentair/messages/controller/pentair_controller_message_status.h"
#include "pentair/messages/pentair_message_ids.h"
#include "pentair/messages/pump/pentair_pump_message_status.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Pentair;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

//=============================================================================
// WU-PENTAIR-EQUIPMENT regression tests:
//   - device discovery validates the wire FROM address (no phantom devices)
//   - the VSP pump watchdog clears stale RPM/Watts/GPM + DataHub flow
//   - the controller treats a 0F reading as sensor-unavailable
//=============================================================================

namespace
{
	constexpr uint8_t CONTROLLER = 0x10;
	constexpr uint8_t CHLORINATOR = 0x50;
	constexpr uint8_t PUMP_60 = 0x60;
	constexpr uint8_t BROADCAST = 0x0F;

	const uint8_t CMD_CHLOR_STATUS = static_cast<uint8_t>(Messages::PentairMessageIds::Chlorinator_Status);
	const uint8_t CMD_CTRL_STATUS = static_cast<uint8_t>(Messages::PentairMessageIds::Controller_Status);
	const uint8_t CMD_PUMP_STATUS = static_cast<uint8_t>(Messages::PentairMessageIds::Pump_Status);

	using CtrlMsg = Messages::PentairControllerMessage_Status;

	std::vector<uint8_t> MakePumpPayload(uint16_t rpm, uint16_t watts, uint8_t gpm, bool running)
	{
		std::vector<uint8_t> data(8, 0);
		data[0] = running ? 0x0A : 0x04;
		data[3] = static_cast<uint8_t>((watts >> 8) & 0xFF);
		data[4] = static_cast<uint8_t>(watts & 0xFF);
		data[5] = static_cast<uint8_t>((rpm >> 8) & 0xFF);
		data[6] = static_cast<uint8_t>(rpm & 0xFF);
		data[7] = gpm;
		return data;
	}

	// Test seam: drive the (otherwise time-driven) watchdog deterministically.
	class WatchdogDrivablePump : public Pentair::Devices::PentairVSPPumpDevice
	{
	public:
		using PentairVSPPumpDevice::PentairVSPPumpDevice;
		using PentairVSPPumpDevice::WatchdogTimeoutOccurred; // make protected override callable from the test.
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_PentairEquipment)

//-----------------------------------------------------------------------------
// FROM-address validation: a chlorinator status frame whose FROM is NOT the
// dedicated chlorinator address must NOT create a device.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Discovery_ChlorinatorFromWrongAddress_IsRejected)
{
	Test::MockReplayHarness harness;

	auto equipment = std::make_unique<Pentair::Equipment::PentairEquipment>(harness.HubLocatorRef());
	harness.EquipmentHub()->AddEquipment(std::move(equipment));

	// CMD says "chlorinator status" but FROM is a pump address (0x60), not 0x50.
	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, CONTROLLER, CMD_CHLOR_STATUS, { 64, 55, 0x00 });
	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	// No device registered at the spoofed address, and none at the real one.
	Pentair::Devices::PentairDeviceId spoofed_id(PUMP_60);
	Pentair::Devices::PentairDeviceId real_id(CHLORINATOR);
	BOOST_CHECK(!harness.EquipmentHub()->DeviceExists(spoofed_id));
	BOOST_CHECK(!harness.EquipmentHub()->DeviceExists(real_id));
}

//-----------------------------------------------------------------------------
// FROM-address validation: a chlorinator status from the correct address still
// registers the device (positive control).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Discovery_ChlorinatorFromCorrectAddress_IsAccepted)
{
	Test::MockReplayHarness harness;

	auto equipment = std::make_unique<Pentair::Equipment::PentairEquipment>(harness.HubLocatorRef());
	harness.EquipmentHub()->AddEquipment(std::move(equipment));

	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		CHLORINATOR, CONTROLLER, CMD_CHLOR_STATUS, { 64, 55, 0x00 });
	harness.Replay(frame);

	Pentair::Devices::PentairDeviceId chlorinator_id(CHLORINATOR);
	BOOST_CHECK(harness.EquipmentHub()->DeviceExists(chlorinator_id));
}

//-----------------------------------------------------------------------------
// FROM-address validation: a controller status frame whose FROM is NOT the
// controller address must NOT create a device.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Discovery_ControllerFromWrongAddress_IsRejected)
{
	Test::MockReplayHarness harness;

	auto equipment = std::make_unique<Pentair::Equipment::PentairEquipment>(harness.HubLocatorRef());
	harness.EquipmentHub()->AddEquipment(std::move(equipment));

	// CMD says "controller status" but FROM is a pump address (0x60), not 0x10.
	std::vector<uint8_t> data = { 12, 0, CtrlMsg::CIRCUIT_POOL, 80, 72, 0 };
	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, BROADCAST, CMD_CTRL_STATUS, data);
	harness.Replay(frame);

	Pentair::Devices::PentairDeviceId spoofed_id(PUMP_60);
	Pentair::Devices::PentairDeviceId real_id(CONTROLLER);
	BOOST_CHECK(!harness.EquipmentHub()->DeviceExists(spoofed_id));
	BOOST_CHECK(!harness.EquipmentHub()->DeviceExists(real_id));
}

//-----------------------------------------------------------------------------
// Watchdog reset: after a status frame populates live RPM/Watts/GPM and the
// DataHub flow rate, a watchdog timeout clears them all back to zero.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Watchdog_ClearsStaleReadingsAndFlow)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<WatchdogDrivablePump>(device_id);

	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, CONTROLLER, CMD_PUMP_STATUS, MakePumpPayload(2750, 1456, 60, true));
	harness.Replay(frame);

	// Sanity: the live operating point is populated and the pump is running.
	BOOST_CHECK_EQUAL(device.ReportedRPM().first, 2750u);
	BOOST_CHECK(device.IsPumpRunning());

	auto pumps_before = harness.DataHub()->Pumps();
	BOOST_REQUIRE_EQUAL(pumps_before.size(), 1u);
	{
		auto status = pumps_before.front()->AuxillaryTraits.TryGet(PumpStatusTrait{});
		BOOST_REQUIRE(status.has_value());
		BOOST_CHECK(status.value() == Kernel::PumpStatuses::Running);
	}

	// Fire the watchdog (as the per-frame PollAll() would on a stopped pump).
	device.WatchdogTimeoutOccurred();

	// Live readings reset.
	BOOST_CHECK_EQUAL(device.ReportedRPM().first, 0u);
	BOOST_CHECK_EQUAL(device.ReportedWatts().first, 0u);
	BOOST_CHECK_EQUAL(device.ReportedGPM().first, 0u);
	BOOST_CHECK(!device.IsPumpRunning());

	// DataHub pump reflects Off + zero flow (no NEW device was created).
	auto pumps_after = harness.DataHub()->Pumps();
	BOOST_REQUIRE_EQUAL(pumps_after.size(), 1u);

	auto status = pumps_after.front()->AuxillaryTraits.TryGet(PumpStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::PumpStatuses::Off);

	auto flow = pumps_after.front()->AuxillaryTraits.TryGet(FlowRateTrait{});
	BOOST_REQUIRE(flow.has_value());
	BOOST_CHECK_CLOSE(flow.value().InGPM().value(), 0.0, 0.01);
}

//-----------------------------------------------------------------------------
// Cached identity: a second status frame reuses the same DataHub pump auxillary
// rather than spawning a duplicate.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Status_TwoFrames_ReuseSameDataHubPump)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<Pentair::Devices::PentairVSPPumpDevice>(device_id);

	harness.Replay(Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, CONTROLLER, CMD_PUMP_STATUS, MakePumpPayload(2000, 1000, 40, true)));
	harness.Replay(Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, CONTROLLER, CMD_PUMP_STATUS, MakePumpPayload(2500, 1200, 50, true)));

	BOOST_CHECK_EQUAL(harness.DataHub()->Pumps().size(), 1u);
	BOOST_CHECK_EQUAL(device.ReportedRPM().first, 2500u);
}

//-----------------------------------------------------------------------------
// Controller 0F sentinel: a 0-degree water temperature is treated as
// sensor-unavailable and is NOT published to the DataHub.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Controller_ZeroDegreeWaterTemp_IsNotPublished)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(CONTROLLER);
	harness.AddDevice<Pentair::Devices::PentairControllerDevice>(device_id);

	// Pool circuit on, water 0F (unavailable), air 70F.
	std::vector<uint8_t> data = { 10, 0, CtrlMsg::CIRCUIT_POOL, 0, 70, 0 };
	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(CONTROLLER, BROADCAST, CMD_CTRL_STATUS, data);
	harness.Replay(frame);

	// The 0F water reading is suppressed.
	BOOST_CHECK(!harness.DataHub()->PoolTemp().has_value());

	// A genuine air reading is still published.
	auto air_temp = harness.DataHub()->AirTemp();
	BOOST_REQUIRE(air_temp.has_value());
	BOOST_CHECK_CLOSE(air_temp.value().InFahrenheit().value(), 70.0, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()
