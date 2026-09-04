#include <cstdint>
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"

#include "pentair/devices/pentair_device_id.h"
#include "pentair/devices/pentair_vsp_pump_device.h"
#include "pentair/messages/pentair_message_ids.h"
#include "pentair/messages/pump/pentair_pump_message_power.h"
#include "pentair/messages/pump/pentair_pump_message_speed.h"

#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Pentair;
using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

//=============================================================================
// Pentair B2: VSP pump (IntelliFlo) decode + command emission + DataHub surface.
//=============================================================================

namespace
{
	constexpr uint8_t PUMP_60 = 0x60;
	constexpr uint8_t CONTROLLER = 0x10;
	const uint8_t CMD_STATUS = static_cast<uint8_t>(Messages::PentairMessageIds::Pump_Status);

	// Build an IntelliFlo status DATA section (8 bytes) for the given live values.
	std::vector<uint8_t> MakeStatusPayload(uint16_t rpm, uint16_t watts, uint8_t gpm, bool running)
	{
		std::vector<uint8_t> data(8, 0);
		data[0] = running ? 0x0A : 0x04;                          // run state
		data[3] = static_cast<uint8_t>((watts >> 8) & 0xFF);      // watts high
		data[4] = static_cast<uint8_t>(watts & 0xFF);             // watts low
		data[5] = static_cast<uint8_t>((rpm >> 8) & 0xFF);        // rpm high
		data[6] = static_cast<uint8_t>(rpm & 0xFF);               // rpm low
		data[7] = gpm;                                            // flow
		return data;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_PentairVSPPump)

//-----------------------------------------------------------------------------
// Decode: a status frame populates the device's RPM/watts/GPM and surfaces a
// Pump auxillary in the DataHub.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_PumpStatus_DecodesAndSurfacesToDataHub)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<Pentair::Devices::PentairVSPPumpDevice>(device_id);

	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		PUMP_60, CONTROLLER, CMD_STATUS, MakeStatusPayload(2750, 1456, 60, true));

	harness.Replay(frame);

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);

	// Device-level decoded state.
	BOOST_CHECK_EQUAL(device.ReportedRPM().first, 2750u);
	BOOST_CHECK_EQUAL(device.ReportedWatts().first, 1456u);
	BOOST_CHECK_EQUAL(device.ReportedGPM().first, 60u);
	BOOST_CHECK(device.IsPumpRunning());

	// Hub-level surfaced pump.
	auto pumps = harness.DataHub()->Pumps();
	BOOST_REQUIRE_EQUAL(pumps.size(), 1u);

	auto status = pumps.front()->AuxillaryTraits.TryGet(PumpStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::PumpStatuses::Running);

	auto type = pumps.front()->AuxillaryTraits.TryGet(PumpTypeTrait{});
	BOOST_REQUIRE(type.has_value());
	BOOST_CHECK(type.value() == Kernel::PumpTypes::FilterCirculation);

	auto speed_kind = pumps.front()->AuxillaryTraits.TryGet(PumpSpeedTrait{});
	BOOST_REQUIRE(speed_kind.has_value());
	BOOST_CHECK(speed_kind.value() == Kernel::PumpSpeeds::VariableSpeed);
}

//-----------------------------------------------------------------------------
// Address filtering: a status frame from a DIFFERENT pump address is ignored by
// this device.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Replay_StatusFromOtherPump_IsIgnored)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<Pentair::Devices::PentairVSPPumpDevice>(device_id);

	// Status from pump 0x61, not 0x60.
	auto frame = Test::PentairMessageBuilder::CreateValidChecksummedFrame(
		0x61, CONTROLLER, CMD_STATUS, MakeStatusPayload(3000, 1500, 50, true));

	harness.Replay(frame);

	BOOST_CHECK_EQUAL(device.ReportedRPM().first, 0u);
	BOOST_CHECK(!device.IsPumpRunning());
	BOOST_CHECK(harness.DataHub()->Pumps().empty());
}

//-----------------------------------------------------------------------------
// Command emission: SetSpeed fires a Speed message with the correct target and
// addressing.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SetSpeed_EmitsSpeedCommand)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<Pentair::Devices::PentairVSPPumpDevice>(device_id);

	std::shared_ptr<const Messages::PentairPumpMessage_Speed> emitted;
	auto connection = Messages::PentairPumpMessage_Speed::GetPublisher()->connect(
		[&emitted](Messages::PentairPumpMessage_Speed::PublisherRef ref)
		{
			emitted = std::make_shared<Messages::PentairPumpMessage_Speed>(ref.get());
		});

	device.SetSpeed(2400);

	connection.disconnect();

	BOOST_REQUIRE(emitted != nullptr);
	BOOST_CHECK_EQUAL(emitted->RPM(), 2400u);
	BOOST_CHECK_EQUAL(emitted->From(), CONTROLLER);
	BOOST_CHECK_EQUAL(emitted->Destination(), PUMP_60);
}

//-----------------------------------------------------------------------------
// Command emission: a Speed command serialises to a frame that decodes back to
// the same target (full round-trip through the real generator).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpeedCommand_SerializesToDecodableFrame)
{
	// Build the command and serialise it as the protocol write path would.
	Messages::PentairPumpMessage_Speed command(CONTROLLER, PUMP_60, 1800);
	std::vector<uint8_t> wire;
	BOOST_REQUIRE(command.Serialize(wire));
	BOOST_REQUIRE(!wire.empty());

	// Replay the serialised bytes back through the decode stack.
	Test::MockReplayHarness harness;

	std::shared_ptr<const Messages::PentairPumpMessage_Speed> decoded;
	auto connection = Messages::PentairPumpMessage_Speed::GetSignal()->connect(
		[&decoded](const Messages::PentairPumpMessage_Speed& msg)
		{
			decoded = std::make_shared<Messages::PentairPumpMessage_Speed>(msg);
		});

	harness.Replay(wire);

	connection.disconnect();

	BOOST_TEST(harness.StatisticsHub()->MessageErrors.ChecksumFailures == 0u);
	BOOST_REQUIRE(decoded != nullptr);
	BOOST_CHECK_EQUAL(decoded->RPM(), 1800u);
	BOOST_CHECK_EQUAL(decoded->From(), CONTROLLER);
	BOOST_CHECK_EQUAL(decoded->Destination(), PUMP_60);
}

//-----------------------------------------------------------------------------
// Command emission: SetPower fires a Power message; its serialised frame decodes
// back to the same on/off state.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SetPower_EmitsPowerCommand_RoundTrips)
{
	Test::MockReplayHarness harness;

	auto device_id = std::make_shared<Pentair::Devices::PentairDeviceId>(PUMP_60);
	auto& device = harness.AddDevice<Pentair::Devices::PentairVSPPumpDevice>(device_id);

	std::shared_ptr<const Messages::PentairPumpMessage_Power> emitted;
	auto connection = Messages::PentairPumpMessage_Power::GetPublisher()->connect(
		[&emitted](Messages::PentairPumpMessage_Power::PublisherRef ref)
		{
			emitted = std::make_shared<Messages::PentairPumpMessage_Power>(ref.get());
		});

	device.SetPower(true);

	connection.disconnect();

	BOOST_REQUIRE(emitted != nullptr);
	BOOST_CHECK(emitted->PowerOn());
	BOOST_CHECK_EQUAL(emitted->Destination(), PUMP_60);

	// Round-trip the serialised power-on frame through the decoder.
	std::vector<uint8_t> wire;
	BOOST_REQUIRE(emitted->Serialize(wire));

	std::shared_ptr<const Messages::PentairPumpMessage_Power> decoded;
	auto rx = Messages::PentairPumpMessage_Power::GetSignal()->connect(
		[&decoded](const Messages::PentairPumpMessage_Power& msg)
		{
			decoded = std::make_shared<Messages::PentairPumpMessage_Power>(msg);
		});

	harness.Replay(wire);
	rx.disconnect();

	BOOST_REQUIRE(decoded != nullptr);
	BOOST_CHECK(decoded->PowerOn());
}

BOOST_AUTO_TEST_SUITE_END()
