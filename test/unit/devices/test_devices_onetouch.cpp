#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "jandy/devices/onetouch_device.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"

#include "kernel/data_hub.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

namespace
{
	struct OneTouchDeviceFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
		OneTouchDeviceFixture()
			: device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x41)))
			, data_hub(Find<Kernel::DataHub>())
		{
		}

		// Add an aux device to the DataHub mimicking what a real iAqualink2 seeds:
		// a Jandy aux id plus a (possibly empty) label.
		void SeedAux(Auxillaries::JandyAuxillaryIds aux_id, const std::string& label)
		{
			auto aux = std::make_shared<Kernel::AuxillaryDevice>();
			aux->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, aux_id);
			if (!label.empty())
			{
				aux->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, label);
			}
			data_hub->Devices.Add(aux);
		}

		std::shared_ptr<JandyDeviceType> device_type;
		std::shared_ptr<Kernel::DataHub> data_hub;
	};
}

BOOST_FIXTURE_TEST_SUITE(OneTouchDevice_TestSuite, OneTouchDeviceFixture)

// =============================================================================
// Construction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_Emulated)
{
	BOOST_CHECK_NO_THROW(OneTouchDevice device(device_type, *this, true));
}

BOOST_AUTO_TEST_CASE(TestConstruction_NonEmulated)
{
	BOOST_CHECK_NO_THROW(OneTouchDevice device(device_type, *this, false));
}

// =============================================================================
// Destruction
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDestruction_CleanAfterConstruction)
{
	{
		OneTouchDevice device(device_type, *this, true);
	}
	BOOST_CHECK(true);
}

// =============================================================================
// Multiple device IDs in the OneTouch range
// =============================================================================

BOOST_AUTO_TEST_CASE(TestConstruction_DifferentDeviceId)
{
	auto device_type_40 = std::make_shared<JandyDeviceType>(JandyDeviceId(0x40));
	BOOST_CHECK_NO_THROW(OneTouchDevice device(device_type_40, *this, true));
}

// =============================================================================
// IAQ-seeded aux label detection (drives the Label Aux scrape skip)
// =============================================================================

// NEGATIVE: with no aux labels on the DataHub (no IAQ on the bus), the emulated
// OneTouch must NOT skip the Label Aux scrape - the full crawl still runs.
BOOST_AUTO_TEST_CASE(TestSeededLabels_NoLabels_PlansFullScrape)
{
	OneTouchDevice device(device_type, *this, true);

	// Nothing seeded yet.
	BOOST_CHECK(!device.DataHubHasSeededAuxLabels());

	// An aux that exists but carries no label is not evidence of seeded labels.
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "");
	BOOST_CHECK(!device.DataHubHasSeededAuxLabels());
}

// POSITIVE: when a real iAqualink2 has seeded an aux label onto the DataHub, the
// emulated OneTouch detects it and will skip the Label Aux scrape.
BOOST_AUTO_TEST_CASE(TestSeededLabels_LabelPresent_SkipsScrape)
{
	OneTouchDevice device(device_type, *this, true);

	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "Pool Light");
	BOOST_CHECK(device.DataHubHasSeededAuxLabels());
}

// A whitespace-only label is treated as no label (not a real seeded name).
BOOST_AUTO_TEST_CASE(TestSeededLabels_WhitespaceLabel_PlansFullScrape)
{
	OneTouchDevice device(device_type, *this, true);

	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_2, "   ");
	BOOST_CHECK(!device.DataHubHasSeededAuxLabels());
}

// One labelled aux among several unlabelled ones is enough to trigger the skip.
BOOST_AUTO_TEST_CASE(TestSeededLabels_OneOfManyLabelled_SkipsScrape)
{
	OneTouchDevice device(device_type, *this, true);

	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_1, "");
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_2, "");
	SeedAux(Auxillaries::JandyAuxillaryIds::Aux_3, "Waterfall");
	BOOST_CHECK(device.DataHubHasSeededAuxLabels());
}

// =============================================================================
// SetpointController capability surface (Set Temperature menu actuation)
//
// SetPoolSetpoint/SetSpaSetpoint enqueue a single menu-edit goal serviced in
// NormalOperation. The synchronous contract just confirms the request was
// accepted/queued; the actual value-stepping is driven later by the Navigator.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSetpoint_Emulated_AcceptsPoolAndSpa)
{
	OneTouchDevice device(device_type, *this, true);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetPoolSetpoint(82)),
		static_cast<int>(Devices::Capabilities::ActuationResult::Accepted));
}

BOOST_AUTO_TEST_CASE(TestSetpoint_NonEmulated_NotSupported)
{
	// A passive (non-emulated) OneTouch cannot transmit, so it cannot edit a setpoint.
	OneTouchDevice device(device_type, *this, false);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetSpaSetpoint(100)),
		static_cast<int>(Devices::Capabilities::ActuationResult::NotSupported));
}

// =============================================================================
// Operating-state gate (regression: false "Applied" while the controller is faulted)
//
// While in ScrapingFaulted / FaultHasOccurred the on-screen state is unknown and a queued
// goal would never be serviced (the per-frame service step runs only in NormalOperation).
// Every actuation path must refuse honestly (NotSupported) in a fault rather than queue a
// goal and falsely report Accepted. A fresh (pre-fault) emulated device still accepts,
// proving it is the fault -- not the request -- that blocks it. (Recovery OUT of these
// states once comms resume is covered separately, below.)
// =============================================================================

namespace
{
	// Exposes the protected test seams so a test can drive the fault states and the
	// comms-resumed recovery path back to NormalOperation.
	struct FaultableOneTouchDevice : public OneTouchDevice
	{
		using OneTouchDevice::OneTouchDevice;
		using OneTouchDevice::ForceScrapingFaultedForTest;
		using OneTouchDevice::ForceFaultHasOccurredForTest;
		using OneTouchDevice::IsInNormalOperationForTest;
		using OneTouchDevice::RenderScreenLineForTest;
		using OneTouchDevice::DeliverStatusFrameForTest;
	};

	// Build a labelled aux that ActuateDevice can target.
	std::shared_ptr<Kernel::AuxillaryDevice> MakeLabelledAux()
	{
		using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;
		auto aux = std::make_shared<Kernel::AuxillaryDevice>();
		aux->AuxillaryTraits.Set(LabelTrait{}, std::string{ "Pool Light" });
		aux->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
		aux->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, Auxillaries::JandyAuxillaryIds::Aux_5);
		return aux;
	}

	// Render a recognised page (PageId::System: line 9 carries "Equipment ON/OFF") so the
	// fault-recovery decision sees a coherent, recognised screen on the next Status frame.
	void RenderRecognisedPage(FaultableOneTouchDevice& device)
	{
		device.RenderScreenLineForTest(9, "Equipment ON/OFF");
	}
}

BOOST_AUTO_TEST_CASE(TestActuate_FaultState_Refused)
{
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	auto aux = std::make_shared<Kernel::AuxillaryDevice>();
	aux->AuxillaryTraits.Set(LabelTrait{}, std::string{ "Pool Light" });
	aux->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Auxillary);
	aux->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, Auxillaries::JandyAuxillaryIds::Aux_5);

	// Fresh emulated device (ColdStart, not faulted): a toggle is accepted/queued -- proving it
	// is the fault, not the request, that blocks the faulted case below.
	FaultableOneTouchDevice fresh(device_type, *this, true);
	BOOST_CHECK(fresh.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);

	// Spa-switch programming on a fresh (non-faulted) emulated panel is likewise accepted -- so the
	// faulted refusal below is the fault gate, not an invalid request. A separate device because
	// 'fresh' now has a toggle goal in flight (one goal at a time on the shared keypad).
	FaultableOneTouchDevice fresh_spaswitch(device_type, *this, true);
	BOOST_CHECK(fresh_spaswitch.SetSpaSwitchAssignment(1, 2, "Pool Light") == Capabilities::ActuationResult::Accepted);

	// Same kind of device, driven into the ScrapingFaulted state: every actuation path refuses
	// honestly. The fault gate runs before any goal is queued, so the calls do not interfere with
	// one another (none of them sets a pending goal).
	FaultableOneTouchDevice faulted(device_type, *this, true);
	faulted.ForceScrapingFaultedForTest();
	BOOST_CHECK(faulted.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(faulted.SetPoolSetpoint(82) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(faulted.SetSpaSetpoint(100) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(faulted.SetChlorinatorPercentage(50) == Capabilities::ActuationResult::NotSupported);
	BOOST_CHECK(faulted.SetChlorinatorBoost(true) == Capabilities::ActuationResult::NotSupported);
	// Regression: SetSpaSwitchAssignment previously guarded on emulation ONLY (not the fault
	// state), so a faulted panel wrongly ACCEPTED and queued a spa-switch goal the
	// NormalOperation-only service step could never run. It must refuse honestly like the rest.
	BOOST_CHECK(faulted.SetSpaSwitchAssignment(1, 2, "Pool Light") == Capabilities::ActuationResult::NotSupported);
}

// =============================================================================
// Fault recovery (regression): a faulted controller is NOT a permanent dead end.
//
// Once ScrapingFaulted / FaultHasOccurred made actuation honestly refuse (NotSupported),
// the device used to stay un-actuatable until process restart. When the controller resumes
// coherent comms (a recognised page on sustained Status frames), the device must recover to
// NormalOperation and accept actuation again. Recovery is gated by hysteresis so a noisy or
// permanently-broken controller never thrashes us out of the fault.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestFaultRecovery_ScrapingFaulted_RecoversOnSustainedComms)
{
	auto aux = MakeLabelledAux();

	FaultableOneTouchDevice device(device_type, *this, true);
	device.ForceScrapingFaultedForTest();

	// While faulted, actuation is honestly refused.
	BOOST_CHECK(!device.IsInNormalOperationForTest());
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);

	// The bus resumes: a recognised page is now displayed. A SINGLE good Status frame must NOT
	// recover us yet -- recovery requires several consecutive good frames (hysteresis/backoff).
	RenderRecognisedPage(device);
	device.DeliverStatusFrameForTest();
	BOOST_CHECK(!device.IsInNormalOperationForTest());
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);

	// Sustained coherent comms cross the threshold (ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES == 3).
	device.DeliverStatusFrameForTest();
	device.DeliverStatusFrameForTest();
	BOOST_CHECK(device.IsInNormalOperationForTest());

	// ...and actuation is accepted again now the device has recovered.
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(TestFaultRecovery_FaultHasOccurred_RecoversOnSustainedComms)
{
	// The watchdog-during-startup fault (FaultHasOccurred) recovers via the same path.
	auto aux = MakeLabelledAux();

	FaultableOneTouchDevice device(device_type, *this, true);
	device.ForceFaultHasOccurredForTest();

	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);

	RenderRecognisedPage(device);
	device.DeliverStatusFrameForTest();
	device.DeliverStatusFrameForTest();
	device.DeliverStatusFrameForTest();

	BOOST_CHECK(device.IsInNormalOperationForTest());
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);
}

BOOST_AUTO_TEST_CASE(TestFaultRecovery_NoRecognisedPage_StaysFaulted)
{
	// Backoff guard: if the controller is talking but NEVER presents a recognised page (line
	// noise / garbled screens), the streak keeps resetting and the device must stay safely
	// faulted -- it never thrashes into NormalOperation and keeps refusing actuation honestly.
	auto aux = MakeLabelledAux();

	FaultableOneTouchDevice device(device_type, *this, true);
	device.ForceScrapingFaultedForTest();

	// An unrecognised screen (no detector matches "????????" on line 0).
	device.RenderScreenLineForTest(0, "????????");
	for (int i = 0; i < 10; ++i)
	{
		device.DeliverStatusFrameForTest();
	}

	BOOST_CHECK(!device.IsInNormalOperationForTest());
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(TestFaultRecovery_FlappingPage_DoesNotRecover)
{
	// Hysteresis requires CONSECUTIVE good frames: a recognised page that flaps in and out
	// (good, unknown, good, unknown ...) never accumulates the streak, so the device stays
	// faulted rather than recovering on a controller that cannot sustain coherent comms.
	auto aux = MakeLabelledAux();

	FaultableOneTouchDevice device(device_type, *this, true);
	device.ForceScrapingFaultedForTest();

	for (int i = 0; i < 6; ++i)
	{
		RenderRecognisedPage(device);          // good page -> streak = 1
		device.DeliverStatusFrameForTest();
		device.RenderScreenLineForTest(9, "");  // clear the detector line -> next frame is Unknown
		device.DeliverStatusFrameForTest();     // resets the streak back to 0
	}

	BOOST_CHECK(!device.IsInNormalOperationForTest());
	BOOST_CHECK(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::NotSupported);
}

BOOST_AUTO_TEST_CASE(TestSetpoint_RejectsSecondGoalWhileBusy)
{
	// One goal at a time: a second request while the first is still pending is rejected
	// so two cursor walks never interleave on the single shared Navigator.
	OneTouchDevice device(device_type, *this, true);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetPoolSetpoint(82)),
		static_cast<int>(Devices::Capabilities::ActuationResult::Accepted));
	BOOST_CHECK_EQUAL(static_cast<int>(device.SetSpaSetpoint(100)),
		static_cast<int>(Devices::Capabilities::ActuationResult::NotSupported));
}

BOOST_AUTO_TEST_CASE(TestSetpoint_ControllerPriority_IsLow)
{
	// OneTouch is a Low-priority controller so a direct-command Serial Adapter (High) and
	// the direct-command AqualinkTouch (Medium) are preferred when present (shared by the
	// DeviceActuator / SetpointController / ChlorinatorController mixins).
	OneTouchDevice device(device_type, *this, true);
	BOOST_CHECK_EQUAL(static_cast<int>(device.ControllerPriority()),
		static_cast<int>(Devices::Capabilities::ActuationPriority::Low));
}

// =============================================================================
// ChlorinatorController capability surface (Set AquaPure % via the menu value
// editor; boost via the Boost Pool page). Verified vs onetouch_chlorinator.cap.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestChlorinator_Emulated_AcceptsPercentageAndBoost)
{
	OneTouchDevice device(device_type, *this, true);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetChlorinatorPercentage(45)),
		static_cast<int>(Devices::Capabilities::ActuationResult::Accepted));
}

BOOST_AUTO_TEST_CASE(TestChlorinatorBoost_Emulated_Accepts)
{
	OneTouchDevice device(device_type, *this, true);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetChlorinatorBoost(true)),
		static_cast<int>(Devices::Capabilities::ActuationResult::Accepted));
}

BOOST_AUTO_TEST_CASE(TestChlorinator_NonEmulated_NotSupported)
{
	// A passive (non-emulated) OneTouch cannot transmit, so it cannot edit the chlorinator.
	OneTouchDevice device(device_type, *this, false);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetChlorinatorPercentage(50)),
		static_cast<int>(Devices::Capabilities::ActuationResult::NotSupported));
	BOOST_CHECK_EQUAL(static_cast<int>(device.SetChlorinatorBoost(false)),
		static_cast<int>(Devices::Capabilities::ActuationResult::NotSupported));
}

BOOST_AUTO_TEST_CASE(TestChlorinator_RejectsWhileAnotherGoalBusy)
{
	// One shared Navigator -> a chlorinator edit is rejected while a setpoint goal is pending.
	OneTouchDevice device(device_type, *this, true);

	BOOST_CHECK_EQUAL(static_cast<int>(device.SetPoolSetpoint(82)),
		static_cast<int>(Devices::Capabilities::ActuationResult::Accepted));
	BOOST_CHECK_EQUAL(static_cast<int>(device.SetChlorinatorPercentage(45)),
		static_cast<int>(Devices::Capabilities::ActuationResult::NotSupported));
}

// =============================================================================
// SanitiseFunctionText: trims surrounding whitespace and non-printable bytes,
// leaving the displayed function/label text exactly (public+static helper).
// =============================================================================

BOOST_AUTO_TEST_CASE(TestSanitise_TrimsSurroundingWhitespace)
{
	BOOST_CHECK_EQUAL(OneTouchDevice::SanitiseFunctionText("   Pool Light   "), "Pool Light");
}

BOOST_AUTO_TEST_CASE(TestSanitise_PreservesInteriorSpaces)
{
	// Only the surrounding run is trimmed; interior spacing is part of the label.
	BOOST_CHECK_EQUAL(OneTouchDevice::SanitiseFunctionText("Equipment ON/OFF"), "Equipment ON/OFF");
}

BOOST_AUTO_TEST_CASE(TestSanitise_StripsControlAndDelBytes)
{
	// Leading/trailing bytes < 0x20 and 0x7f (DEL) are trimmed like whitespace.
	const std::string raw = std::string("\x01\x02") + "Spa" + std::string("\x7f\x1f");
	BOOST_CHECK_EQUAL(OneTouchDevice::SanitiseFunctionText(raw), "Spa");
}

BOOST_AUTO_TEST_CASE(TestSanitise_AllWhitespaceYieldsEmpty)
{
	BOOST_CHECK(OneTouchDevice::SanitiseFunctionText("     ").empty());
	BOOST_CHECK(OneTouchDevice::SanitiseFunctionText("").empty());
}

// =============================================================================
// AvailableFunctions: the canonical spa-switch picker list. Model-agnostic
// assertions (non-empty, already-sanitised, no duplicates) so the test does not
// pin the exact catalogue.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestAvailableFunctions_IsNonEmptyAndSanitised)
{
	OneTouchDevice device(device_type, *this, true);

	const auto functions = device.AvailableFunctions();
	BOOST_REQUIRE(!functions.empty());

	std::set<std::string> seen;
	for (const auto& fn : functions)
	{
		BOOST_CHECK(!fn.empty());
		// Each entry is already in its sanitised (trimmed) form.
		BOOST_CHECK_EQUAL(fn, OneTouchDevice::SanitiseFunctionText(fn));
		// No duplicates: the picker cycles a set, not a list with repeats.
		BOOST_CHECK_MESSAGE(seen.insert(fn).second, "duplicate function in catalogue: " + fn);
	}
}

// =============================================================================
// DescribeDiagnostics: the JSON surface served at /api/diagnostics. A fresh
// emulated device exposes its identity, the ColdStart operating state, and the
// navigator / spider-engine sub-objects (both are constructed up front).
// =============================================================================

BOOST_AUTO_TEST_CASE(TestDiagnostics_FreshEmulatedDeviceShape)
{
	OneTouchDevice device(device_type, *this, true);

	const auto diag = device.DescribeDiagnostics();

	BOOST_CHECK_EQUAL(diag.at("device_type").get<std::string>(), "OneTouch");
	BOOST_CHECK_EQUAL(diag.at("device_id").get<std::string>(), "0x41");
	BOOST_CHECK_EQUAL(diag.at("operating_state").get<std::string>(), "ColdStart");
	BOOST_CHECK_EQUAL(diag.at("is_emulated").get<bool>(), true);

	// Sub-objects / scalars every OneTouch diagnostic carries.
	BOOST_CHECK(diag.contains("screen"));
	BOOST_CHECK(diag.contains("navigator"));
	BOOST_CHECK(diag.contains("spider_engine"));
	BOOST_CHECK(diag.contains("scraping_stall_counter"));
	BOOST_CHECK(diag.contains("highlighted_line"));
	BOOST_CHECK(diag.contains("pending_key_command"));
	BOOST_CHECK(diag.contains("is_running"));
}

BOOST_AUTO_TEST_CASE(TestDiagnostics_NonEmulatedFlag)
{
	OneTouchDevice device(device_type, *this, false);

	const auto diag = device.DescribeDiagnostics();
	BOOST_CHECK_EQUAL(diag.at("is_emulated").get<bool>(), false);
}

// =============================================================================
// Operating-state machine driven through the screen seams (RenderScreenLineForTest
// presents a page; DeliverStatusFrameForTest runs one real ProcessControllerUpdates).
// Covers the ColdStart page-detection branches and the non-emulated fast-track.
// =============================================================================

namespace
{
	std::string OperatingStateOf(const OneTouchDevice& device)
	{
		return device.DescribeDiagnostics().at("operating_state").get<std::string>();
	}

	// Present the StartUp splash (cold-start screen): line 7 "REV ..." + line 5 "-..."
	// match the StartUp detector, which is transient so the device waits on it.
	void RenderStartUpSplash(FaultableOneTouchDevice& device)
	{
		device.RenderScreenLineForTest(5, "-RS8");
		device.RenderScreenLineForTest(7, "REV T.0");
	}
}

BOOST_AUTO_TEST_CASE(TestState_ColdStart_StaysOnTransientSplash)
{
	// The splash is a recognised-but-transient page: the device must remain in ColdStart
	// and wait for the controller to advance, rather than starting the spider crawl.
	FaultableOneTouchDevice device(device_type, *this, true);
	BOOST_REQUIRE_EQUAL(OperatingStateOf(device), "ColdStart");

	RenderStartUpSplash(device);
	device.DeliverStatusFrameForTest();

	BOOST_CHECK_EQUAL(OperatingStateOf(device), "ColdStart");
}

BOOST_AUTO_TEST_CASE(TestState_ColdStart_StaysOnUnrecognisedPage)
{
	// An unrecognised screen (no detector matches) keeps the device waiting in ColdStart.
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "????????");
	device.DeliverStatusFrameForTest();

	BOOST_CHECK_EQUAL(OperatingStateOf(device), "ColdStart");
}

BOOST_AUTO_TEST_CASE(TestState_ColdStart_AdvancesOnRecognisedPage)
{
	// A recognised, non-transient page (System: line 9 "Equipment ON/OFF") means the
	// controller is past the splash, so the device leaves ColdStart and begins scraping.
	FaultableOneTouchDevice device(device_type, *this, true);

	RenderRecognisedPage(device);
	device.DeliverStatusFrameForTest();

	BOOST_CHECK_NE(OperatingStateOf(device), "ColdStart");
}

BOOST_AUTO_TEST_CASE(TestState_NonEmulated_FastTracksToNormalOperation)
{
	// A passive (non-emulated) device never runs the spider crawl: the first controller
	// update drops it straight into NormalOperation so it can observe screens.
	FaultableOneTouchDevice device(device_type, *this, false);

	device.DeliverStatusFrameForTest();

	BOOST_CHECK_EQUAL(OperatingStateOf(device), "NormalOperation");
	BOOST_CHECK(device.IsInNormalOperationForTest());
}

// =============================================================================
// Goal servicing in NormalOperation: once a recovered emulated device accepts a
// goal, subsequent Status frames run the matching per-frame ProcessStep (the
// navigating phase). With the screen held on a single page the navigator cannot
// complete, but the device stays coherently in NormalOperation and never throws.
// =============================================================================

namespace
{
	// Drive an emulated device into NormalOperation via the fault-recovery path (the only
	// deterministic, crawl-free way to reach NormalOperation on an emulated device).
	FaultableOneTouchDevice& RecoverToNormalOperation(FaultableOneTouchDevice& device)
	{
		device.ForceScrapingFaultedForTest();
		RenderRecognisedPage(device);
		for (uint32_t i = 0; i < 3; ++i)  // ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES
		{
			device.DeliverStatusFrameForTest();
		}
		return device;
	}
}

BOOST_AUTO_TEST_CASE(TestGoal_SetpointEdit_ServicedAcrossFrames)
{
	FaultableOneTouchDevice device(device_type, *this, true);
	RecoverToNormalOperation(device);
	BOOST_REQUIRE(device.IsInNormalOperationForTest());

	BOOST_REQUIRE(device.SetPoolSetpoint(82) == Capabilities::ActuationResult::Accepted);

	// Each Status frame advances the value-edit ProcessStep's navigating phase.
	for (int i = 0; i < 8; ++i)
	{
		BOOST_CHECK_NO_THROW(device.DeliverStatusFrameForTest());
	}
	BOOST_CHECK(device.IsInNormalOperationForTest());
}

BOOST_AUTO_TEST_CASE(TestGoal_Actuation_ServicedAcrossFrames)
{
	auto aux = MakeLabelledAux();

	FaultableOneTouchDevice device(device_type, *this, true);
	RecoverToNormalOperation(device);
	BOOST_REQUIRE(device.IsInNormalOperationForTest());

	BOOST_REQUIRE(device.ActuateDevice(aux, Capabilities::ActuationAction::Toggle) == Capabilities::ActuationResult::Accepted);

	for (int i = 0; i < 8; ++i)
	{
		BOOST_CHECK_NO_THROW(device.DeliverStatusFrameForTest());
	}
	BOOST_CHECK(device.IsInNormalOperationForTest());
}

BOOST_AUTO_TEST_CASE(TestGoal_ChlorinatorBoost_ServicedAcrossFrames)
{
	FaultableOneTouchDevice device(device_type, *this, true);
	RecoverToNormalOperation(device);
	BOOST_REQUIRE(device.IsInNormalOperationForTest());

	BOOST_REQUIRE(device.SetChlorinatorBoost(true) == Capabilities::ActuationResult::Accepted);

	for (int i = 0; i < 8; ++i)
	{
		BOOST_CHECK_NO_THROW(device.DeliverStatusFrameForTest());
	}
	BOOST_CHECK(device.IsInNormalOperationForTest());
}

// =============================================================================
// Equipment Status page processing (StatusProcessor_* fan-out)
//
// Rendering the EQUIPMENT STATUS page and completing it with a Status frame runs
// PageProcessor_EquipmentStatus, which dispatches every non-empty line through
// the nine StatusProcessor_* matchers. Each decodes its row into the DataHub.
// =============================================================================

namespace
{
	// Complete the rendered screen so ProcessScreenUpdates() runs the matching page
	// processor (the render seam leaves the screen in Updating mode; the page
	// processors only fire on the UpdateComplete transition, exactly as a real
	// Status frame would drive it). Screen mode + ProcessScreenUpdates are public.
	void CompleteScreen(OneTouchDevice& device)
	{
		device.ScreenMode(Capabilities::ScreenModes::UpdateComplete);
		device.ProcessScreenUpdates();
	}
}

BOOST_AUTO_TEST_CASE(TestStatusProcessor_FilterPump_CreatesRunningPump)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
	device.RenderScreenLineForTest(2, "Filter Pump");
	CompleteScreen(device);

	auto pumps = data_hub->FilterPumps();
	BOOST_REQUIRE_EQUAL(pumps.size(), 1u);
	auto status = pumps.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::PumpStatuses::Running);
}

BOOST_AUTO_TEST_CASE(TestStatusProcessor_PoolHeat_EnaParsesEnabled)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
	device.RenderScreenLineForTest(3, "Pool Heat ENA");   // "ENA" suffix -> Enabled
	CompleteScreen(device);

	auto heaters = data_hub->Devices.FindByLabel("Pool Heat");
	BOOST_REQUIRE_EQUAL(heaters.size(), 1u);
	auto status = heaters.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Enabled);
}

BOOST_AUTO_TEST_CASE(TestStatusProcessor_SpaHeat_NoSuffixParsesHeating)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
	device.RenderScreenLineForTest(4, "Spa Heat");   // no "ENA" -> actively Heating
	CompleteScreen(device);

	auto heaters = data_hub->Devices.FindByLabel("Spa Heat");
	BOOST_REQUIRE_EQUAL(heaters.size(), 1u);
	auto status = heaters.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{});
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(TestStatusProcessor_AquaPure_CreatesChlorinatorWithDutyCycle)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
	device.RenderScreenLineForTest(6, "AquaPure  50%");   // right-justified value (multiple spaces)
	CompleteScreen(device);

	auto chlorinators = data_hub->Devices.FindByLabel("AquaPure");
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);
	auto duty = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::DutyCycleTrait{});
	BOOST_REQUIRE(duty.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(duty.value()), 50);
}

BOOST_AUTO_TEST_CASE(TestStatusProcessor_SaltLevel_ParsesPpm)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
	device.RenderScreenLineForTest(7, "Salt  3000  ppm");
	CompleteScreen(device);

	BOOST_CHECK_CLOSE(data_hub->SaltLevel().value(), 3000.0, 0.01);
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_Service_SetsServiceMode)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	// The Service page is detected by line 3 "Service Mode".
	device.RenderScreenLineForTest(3, "Service Mode");
	CompleteScreen(device);

	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::Service);
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_TimeOut_SetsTimeOutMode)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	// The TimeOut page is detected by line 3 "Timeout Mode".
	device.RenderScreenLineForTest(3, "Timeout Mode");
	CompleteScreen(device);

	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::TimeOut);
}

// =============================================================================
// Configuration / setpoint page processors (decode a scraped menu page into the
// DataHub model). Same CompleteScreen() mechanism as the Equipment Status tests.
// =============================================================================

BOOST_AUTO_TEST_CASE(TestPageProcessor_SetTemperature_ParsesHeatSetpoints)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "    Set Temp");          // detector
	device.RenderScreenLineForTest(2, "Pool Heat   90`F");
	device.RenderScreenLineForTest(3, "Spa Heat   102`F");
	CompleteScreen(device);

	auto pool = data_hub->PoolTempSetpoint();
	auto spa = data_hub->SpaTempSetpoint();
	BOOST_REQUIRE(pool.has_value());
	BOOST_REQUIRE(spa.has_value());
	BOOST_CHECK_CLOSE(pool.value().InFahrenheit().value(), 90.0, 0.5);
	BOOST_CHECK_CLOSE(spa.value().InFahrenheit().value(), 102.0, 0.5);
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_SetAquapure_ScrapesChlorinatorSetpoints)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "  Set AQUAPURE");        // detector
	device.RenderScreenLineForTest(3, "Set Pool to:  45%");
	device.RenderScreenLineForTest(4, "Set Spa to:   50%");
	CompleteScreen(device);

	auto chlorinators = data_hub->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);
	auto pool_sp = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorPoolSetpointTrait{});
	auto spa_sp = chlorinators.front()->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::ChlorinatorSpaSetpointTrait{});
	BOOST_REQUIRE(pool_sp.has_value());
	BOOST_REQUIRE(spa_sp.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(pool_sp.value()), 45);
	BOOST_CHECK_EQUAL(static_cast<int>(spa_sp.value()), 50);
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_Version_PopulatesVersionsAndBodies)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(4, "B0029221");
	device.RenderScreenLineForTest(5, "RS-8 Combo");
	device.RenderScreenLineForTest(7, "REV T.0.1");           // detector "REV "
	CompleteScreen(device);

	BOOST_CHECK_EQUAL(data_hub->EquipmentVersions.Get("Model"), "B0029221");
	BOOST_CHECK_EQUAL(data_hub->EquipmentVersions.Get("Type"), "RS-8 Combo");
	BOOST_CHECK_EQUAL(data_hub->EquipmentVersions.Get("Revision"), "REV T.0.1");
	// A decoded panel type resolves a pool configuration, which seeds the bodies.
	BOOST_CHECK(data_hub->PoolConfiguration != Kernel::PoolConfigurations::Unknown);
	BOOST_CHECK(!data_hub->Bodies().empty());
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_FreezeProtect_ParsesThreshold)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, " Freeze Protect");      // detector
	device.RenderScreenLineForTest(3, "Temp        38`F");
	CompleteScreen(device);

	auto threshold = data_hub->FreezeProtectPoint();
	BOOST_REQUIRE(threshold.has_value());
	BOOST_CHECK_CLOSE(threshold.value().InFahrenheit().value(), 38.0, 0.5);
}

BOOST_AUTO_TEST_CASE(TestPageProcessor_EquipmentOnOff_CreatesAuxDevices)
{
	FaultableOneTouchDevice device(device_type, *this, true);

	device.RenderScreenLineForTest(0, "Filter Pump  ***");     // detector "Filter Pump"
	device.RenderScreenLineForTest(5, "Aux1         OFF");
	device.RenderScreenLineForTest(6, "Aux2         OFF");
	device.RenderScreenLineForTest(7, "Aux3          ON");
	CompleteScreen(device);

	// Each aux row is decoded into an auxillary device on the DataHub.
	BOOST_CHECK_GE(data_hub->Auxillaries().size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()
