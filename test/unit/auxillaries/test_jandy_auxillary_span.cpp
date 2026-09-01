#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_reconciliation.h"
#include "jandy/auxillaries/jandy_auxillary_span.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/device_graph/device_graph.h"
#include "kernel/system_boards.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

//=============================================================================
// AuxillaryModelSpan - which aux relays the DETECTED PANEL MODEL can have.
//
// REGRESSION CONTEXT: the emulated Serial Adapter round-robins a status query
// over every aux id the protocol can name, and a reply used to be treated as
// proof the relay exists. On a single-power-centre panel (an RS-8 Combo: 7 aux
// relays, 1 centre) that minted a generically-labelled device for every slot in
// banks B/C/D -- "Aux B1", "Aux D6" -- none of which the panel has. The span
// makes the sweep evidence-driven using the model the panel itself reports, with
// nothing hardcoded to a particular installation. It judges NUMBERED relays only.
//=============================================================================

namespace
{
	using enum Auxillaries::JandyAuxillaryIds;

	std::shared_ptr<Kernel::AuxillaryDevice> AddAux(Kernel::DevicesGraph& devices, Auxillaries::JandyAuxillaryIds id, const std::string& label)
	{
		auto device = std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(id));
		device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		device->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		Auxillaries::EnsureAuxIdentity(device, id);
		devices.Add(device);
		return device;
	}

	// A cache-restored placeholder: the stable id and labels survive, but the cache is
	// protocol-agnostic so the JandyAuxillaryId trait does NOT.
	std::shared_ptr<Kernel::AuxillaryDevice> AddCachePlaceholder(Kernel::DevicesGraph& devices, Auxillaries::JandyAuxillaryIds id, const std::string& label)
	{
		auto device = std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(id));
		device->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		device->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		devices.Add(device);
		return device;
	}
}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(AuxillaryModelSpan_TestSuite)

BOOST_AUTO_TEST_CASE(UnknownSpanContainsEverything)
{
	// Nothing has been decoded yet, so nothing may be excluded -- an RSSA-only rig has no
	// other enumerating source and must still be able to sweep for relays.
	const Auxillaries::AuxillaryModelSpan span;

	BOOST_CHECK(!span.IsKnown());
	BOOST_CHECK(span.Contains(Aux_1));
	BOOST_CHECK(span.Contains(Aux_B1));
	BOOST_CHECK(span.Contains(Aux_D8));
	BOOST_CHECK(span.Contains(ExtraAux));
}

BOOST_AUTO_TEST_CASE(SinglePowerCentreModelExcludesRemoteBanks)
{
	// RS-8 Combo: 7 aux relays across 1 power centre.
	const Auxillaries::AuxillaryModelSpan span{ 1, 7 };

	BOOST_REQUIRE(span.IsKnown());

	for (const auto id : { Aux_1, Aux_2, Aux_3, Aux_4, Aux_5, Aux_6, Aux_7 })
	{
		BOOST_CHECK(span.Contains(id));
	}

	// Banks B/C/D live on power centres this model does not have.
	for (const auto id : { Aux_B1, Aux_B8, Aux_C1, Aux_C8, Aux_D1, Aux_D6, Aux_D8 })
	{
		BOOST_CHECK(!span.Contains(id));
	}
}

BOOST_AUTO_TEST_CASE(SinglePowerCentreModelExcludesRelaysAboveItsCount)
{
	// RS-4 Only: 3 aux relays, 1 power centre. The total IS centre A's count when there is
	// only one centre, so Aux4+ are relays this panel does not have.
	const Auxillaries::AuxillaryModelSpan span{ 1, 3 };

	BOOST_CHECK(span.Contains(Aux_1));
	BOOST_CHECK(span.Contains(Aux_3));
	BOOST_CHECK(!span.Contains(Aux_4));
	BOOST_CHECK(!span.Contains(Aux_7));
}

BOOST_AUTO_TEST_CASE(MultiCentreModelKeepsItsBanksAndDoesNotUseTheTotalWithinACentre)
{
	// RS-2/10 Dual: 10 relays over 2 centres, split A=6 + B=4 -- NOT A=7 + B=3. The total
	// therefore cannot bound an individual centre, so within-centre trimming must not apply
	// and every relay of both banks stays reachable.
	const Auxillaries::AuxillaryModelSpan span{ 2, 10 };

	BOOST_CHECK(span.Contains(Aux_1));
	BOOST_CHECK(span.Contains(Aux_7));
	BOOST_CHECK(span.Contains(Aux_B1));
	BOOST_CHECK(span.Contains(Aux_B8));

	// Centres C and D still do not exist on a two-centre model.
	BOOST_CHECK(!span.Contains(Aux_C1));
	BOOST_CHECK(!span.Contains(Aux_D8));
}

BOOST_AUTO_TEST_CASE(ExtraAuxIsAlwaysAdmitted)
{
	// ExtraAux belongs to no numbered power centre and the model tables count only numbered
	// relays, so a power-centre span has NO opinion about it -- and "no opinion" must mean
	// keep. It is a real relay on panels that have one (the AquaLink RS manual describes it as
	// the solar booster pump whenever solar heating is enabled), so a relay-count table is not
	// grounds to delete it.
	const Auxillaries::AuxillaryModelSpan single_centre{ 1, 7 };
	const Auxillaries::AuxillaryModelSpan four_centres{ 4, 31 };

	BOOST_CHECK(single_centre.Contains(ExtraAux));
	BOOST_CHECK(four_centres.Contains(ExtraAux));
}

BOOST_AUTO_TEST_CASE(FromDataHubIsUnknownUntilThePanelModelIsIdentified)
{
	Kernel::DataHub data_hub;

	// Nothing decoded.
	BOOST_CHECK(!Auxillaries::AuxillaryModelSpan::FromDataHub(data_hub).IsKnown());

	// Counts present but the panel type was NOT recognised: PoolConfigurationDecoder falls back
	// to a placeholder (1 aux / 1 centre) for an unknown type, so trusting the counts alone
	// would wrongly trim a real multi-centre panel whose type string we simply cannot read.
	data_hub.ExpectedAuxillaryCount = 1;
	data_hub.ExpectedPowerCenterCount = 1;
	BOOST_CHECK(!Auxillaries::AuxillaryModelSpan::FromDataHub(data_hub).IsKnown());

	// Identified model: the span becomes usable.
	data_hub.SystemBoard = Kernel::SystemBoards::RS8_Combo;
	data_hub.ExpectedAuxillaryCount = 7;
	data_hub.ExpectedPowerCenterCount = 1;

	const auto span = Auxillaries::AuxillaryModelSpan::FromDataHub(data_hub);
	BOOST_REQUIRE(span.IsKnown());
	BOOST_CHECK(span.Contains(Aux_7));
	BOOST_CHECK(!span.Contains(Aux_B1));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// PruneAuxillariesOutsideSpan - clears phantoms already in the hub, including
// ones restored from an equipment cache written before the sweep was bounded.
//=============================================================================

BOOST_AUTO_TEST_SUITE(PruneAuxillariesOutsideSpan_TestSuite)

BOOST_AUTO_TEST_CASE(RemovesRelaysTheModelCannotHaveAndKeepsTheRest)
{
	Kernel::DevicesGraph devices;

	AddAux(devices, Aux_1, "Spa Jets");
	AddAux(devices, Aux_5, "Pool Light");
	AddAux(devices, Aux_7, "Spillway");
	AddAux(devices, Aux_B1, "Aux B1");
	AddAux(devices, Aux_B2, "Aux B2");
	AddAux(devices, Aux_D6, "Aux D6");
	AddAux(devices, ExtraAux, "Extra Aux");

	const auto removed = Auxillaries::PruneAuxillariesOutsideSpan(devices, Auxillaries::AuxillaryModelSpan{ 1, 7 });

	BOOST_CHECK_EQUAL(removed, 3u);
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_1)));
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_5)));
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_7)));
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_B1)));
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_B2)));
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_D6)));
	// ExtraAux is outside no power centre the model declares -- the span has no opinion, so it
	// survives (whether a given panel really has one is a separate, capture-gated question).
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(ExtraAux)));
}

BOOST_AUTO_TEST_CASE(RemovesCacheRestoredPhantomsWithoutTheProtocolTrait)
{
	// A phantom persisted by an earlier run comes back WITHOUT JandyAuxillaryId, so it has to
	// be recognised from its label. Without this it would outlive the fix in every install
	// that already has one on disk.
	Kernel::DevicesGraph devices;

	AddCachePlaceholder(devices, Aux_B3, "Aux B3");
	AddCachePlaceholder(devices, Aux_2, "Swim Jet");

	const auto removed = Auxillaries::PruneAuxillariesOutsideSpan(devices, Auxillaries::AuxillaryModelSpan{ 1, 7 });

	BOOST_CHECK_EQUAL(removed, 1u);
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_B3)));
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_2)));
}

BOOST_AUTO_TEST_CASE(KeepsAnOutOfSpanRelayThatSomethingActuallyNamed)
{
	// Safety valve. A phantom minted by the blind status sweep has no label and falls back to
	// the generic enum name; a relay carrying an OPERATOR-ASSIGNED name was named by something
	// that enumerates real equipment. If the two disagree with the model table, believe the
	// name -- deleting a device the user can see on their panel is far worse than leaving one
	// the validator will flag as an anomaly.
	Kernel::DevicesGraph devices;

	AddAux(devices, Aux_6, "Air Blower");
	AddAux(devices, Aux_B4, "Garden Lights");   // out of span, but named
	AddAux(devices, Aux_B5, "Aux B5");          // out of span, never named -> a phantom

	const auto removed = Auxillaries::PruneAuxillariesOutsideSpan(devices, Auxillaries::AuxillaryModelSpan{ 1, 7 });

	BOOST_CHECK_EQUAL(removed, 1u);
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_6)));
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_B4)));
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_B5)));
}

BOOST_AUTO_TEST_CASE(UnknownSpanRemovesNothing)
{
	Kernel::DevicesGraph devices;

	AddAux(devices, Aux_1, "Spa Jets");
	AddAux(devices, Aux_B1, "Aux B1");

	BOOST_CHECK_EQUAL(Auxillaries::PruneAuxillariesOutsideSpan(devices, Auxillaries::AuxillaryModelSpan{}), 0u);
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_B1)));
}

BOOST_AUTO_TEST_CASE(LeavesNonAuxillaryEquipmentAlone)
{
	// Pumps / heaters / the chlorinator carry no aux id; the prune must not touch them.
	Kernel::DevicesGraph devices;

	auto pump = std::make_shared<Kernel::AuxillaryDevice>();
	pump->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Pump);
	pump->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Filter Pump" });
	devices.Add(pump);

	auto unnamed_aux = std::make_shared<Kernel::AuxillaryDevice>();
	unnamed_aux->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
	unnamed_aux->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Waterfall" });
	devices.Add(unnamed_aux);

	const Auxillaries::AuxillaryModelSpan span{ 1, 7 };
	BOOST_CHECK_EQUAL(Auxillaries::PruneAuxillariesOutsideSpan(devices, span), 0u);
	BOOST_CHECK(nullptr != devices.FindById(pump->Id()));
	BOOST_CHECK(nullptr != devices.FindById(unnamed_aux->Id()));
}

BOOST_AUTO_TEST_SUITE_END()
