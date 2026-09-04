#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/devices/aquarite_device.h"
#include "jandy/devices/pda_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/messages/jandy_message_ids.h"

#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

#include "support/unit_test_hublocatorinjector.h"
#include "support/unit_test_mockreplayharness.h"
#include "support/unit_test_ostream_support.h"
#include "support/unit_test_protocolmessagebuilder.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// Small-device branches the per-device suites leave untouched: the PDA's
// diagnostics projection (its device card's whole payload) and the AquaRite's
// severity ranking across a status byte carrying several simultaneous flags.
//=============================================================================

namespace
{
	constexpr uint8_t PDA_DEVICE_ID{ 0x60 };
	constexpr uint8_t AQUARITE_DEVICE_ID{ 0x50 };
	constexpr uint8_t AQUARITE_PPM_DEST{ 0x00 };   // AQUARITE_PPM is sent SWG -> Master.

	struct MiscDeviceFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		std::shared_ptr<JandyDeviceType> Id(uint8_t address) const
		{
			return std::make_shared<JandyDeviceType>(JandyDeviceId(address));
		}
	};

	std::vector<uint8_t> MakePpmFrame(uint8_t salt_byte, uint8_t status_byte)
	{
		const auto command = static_cast<uint8_t>(Messages::JandyMessageIds::AQUARITE_PPM);
		return Test::MessageBuilder::CreateValidChecksummedMessage(AQUARITE_PPM_DEST, command, { salt_byte, status_byte });
	}
}
// unnamed namespace

//=============================================================================
BOOST_FIXTURE_TEST_SUITE(PDADeviceBranches_TestSuite, MiscDeviceFixture)
//=============================================================================

BOOST_AUTO_TEST_CASE(Diagnostics_CarryTheDeviceIdentityScreenAndEmulationState)
{
	// The diagnostics payload is the entire content of the PDA's device card; every field it
	// promises has to be present, or the card renders blank.
	PDADevice device(Id(PDA_DEVICE_ID), *this, /*is_emulated=*/false);

	const auto diagnostics = device.DescribeDiagnostics();

	BOOST_CHECK_EQUAL(diagnostics["device_type"].get<std::string>(), std::string("PDA"));
	BOOST_CHECK_EQUAL(diagnostics["device_id"].get<std::string>(), std::string("0x60"));
	BOOST_REQUIRE(diagnostics.contains("screen"));
	BOOST_CHECK(diagnostics["screen"].contains("page_type"));
	BOOST_CHECK(diagnostics["screen"].contains("lines"));
	BOOST_CHECK(!diagnostics["scrape_state"].get<std::string>().empty());
	BOOST_CHECK_EQUAL(diagnostics["is_emulated"].get<bool>(), false);
	BOOST_CHECK(diagnostics["emulation_suppressed"].is_boolean());
	BOOST_CHECK(diagnostics.contains("is_running"));
}

BOOST_AUTO_TEST_CASE(Diagnostics_ReportAnEmulatedPDAAsEmulated)
{
	PDADevice device(Id(0x61), *this, /*is_emulated=*/true);

	const auto diagnostics = device.DescribeDiagnostics();
	BOOST_CHECK_EQUAL(diagnostics["device_id"].get<std::string>(), std::string("0x61"));
	BOOST_CHECK_EQUAL(diagnostics["is_emulated"].get<bool>(), true);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
BOOST_AUTO_TEST_SUITE(AquariteDeviceBranches_TestSuite)
//=============================================================================

BOOST_AUTO_TEST_CASE(ManySimultaneousWarnings_ResolveToTheHighestSeverityFlag)
{
	// The status byte is a bitfield, so a cell in trouble can raise several warnings at once.
	// The single health badge must show the WORST of them -- electrical/hardware safety outranks
	// chemistry, which outranks routine maintenance -- while the full set stays visible.
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(AQUARITE_DEVICE_ID));
	harness.AddDevice<AquariteDevice>(device_id);

	// 0x7f = NoFlow|LowSalt|HighSalt|CleanCell|HighCurrent|LowVoltage|LowTemperature.
	harness.Replay(MakePpmFrame(0x28, 0x7f));

	auto chlorinators = harness.DataHub()->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);

	auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
	BOOST_REQUIRE(health.has_value());
	BOOST_CHECK_EQUAL(health.value(), Kernel::ChlorinatorHealth::Warning_HighCurrent);

	auto flags = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthFlagsTrait{});
	BOOST_REQUIRE(flags.has_value());
	const std::set<Kernel::ChlorinatorHealth> expected{
		Kernel::ChlorinatorHealth::Warning_NoFlow,
		Kernel::ChlorinatorHealth::Warning_LowSalt,
		Kernel::ChlorinatorHealth::Warning_HighSalt,
		Kernel::ChlorinatorHealth::Warning_CleanCell,
		Kernel::ChlorinatorHealth::Warning_HighCurrent,
		Kernel::ChlorinatorHealth::Warning_LowVoltage,
		Kernel::ChlorinatorHealth::Warning_LowTemperature
	};
	BOOST_CHECK(flags.value() == expected);
}

BOOST_AUTO_TEST_CASE(AHardwareFaultAlongsideWarnings_TakesThePrimaryHealthSlot)
{
	// The same ranking, with a hardware fault bit added: it must displace the electrical warning
	// that won above. Raising a more severe state is deliberately fast (no dwell delay).
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(AQUARITE_DEVICE_ID));
	harness.AddDevice<AquariteDevice>(device_id);

	// 0x91 = Error_CheckPCB|Warning_HighCurrent|Warning_NoFlow.
	harness.Replay(MakePpmFrame(0x28, 0x91));

	auto chlorinators = harness.DataHub()->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);

	auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
	BOOST_REQUIRE(health.has_value());
	BOOST_CHECK_EQUAL(health.value(), Kernel::ChlorinatorHealth::Error_CheckPCB);
}

BOOST_AUTO_TEST_CASE(SaltAndChemistryWarnings_RankBelowElectricalOnes)
{
	// Chemistry outranks routine maintenance: a high-salt reading alongside a clean-cell
	// reminder must surface as the salt problem, not the reminder.
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(AQUARITE_DEVICE_ID));
	harness.AddDevice<AquariteDevice>(device_id);

	// 0x0c = Warning_HighSalt(0x04) | Warning_CleanCell(0x08).
	harness.Replay(MakePpmFrame(0x28, 0x0c));

	auto chlorinators = harness.DataHub()->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);

	auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
	BOOST_REQUIRE(health.has_value());
	BOOST_CHECK_EQUAL(health.value(), Kernel::ChlorinatorHealth::Warning_HighSalt);
}

BOOST_AUTO_TEST_CASE(LowSaltAlongsideALowTemperatureReminder_ShowsTheSaltProblem)
{
	// Ranks 6 (low salt) vs 8 (low temperature): the chemistry problem the operator can act on
	// wins over the cell's cold-water derate notice.
	Test::MockReplayHarness harness;
	auto device_id = std::make_shared<JandyDeviceType>(JandyDeviceId(AQUARITE_DEVICE_ID));
	harness.AddDevice<AquariteDevice>(device_id);

	// 0x42 = Warning_LowTemperature(0x40) | Warning_LowSalt(0x02).
	harness.Replay(MakePpmFrame(0x1d, 0x42));

	auto chlorinators = harness.DataHub()->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);

	auto health = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{});
	BOOST_REQUIRE(health.has_value());
	BOOST_CHECK_EQUAL(health.value(), Kernel::ChlorinatorHealth::Warning_LowSalt);
}

BOOST_AUTO_TEST_SUITE_END()
