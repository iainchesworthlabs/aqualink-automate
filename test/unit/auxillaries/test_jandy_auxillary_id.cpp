#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <magic_enum/magic_enum.hpp>
#include <boost/uuid/uuid.hpp>

#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_reconciliation.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/stable_id.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/device_graph/device_graph.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

//=============================================================================
// ParseAuxId - tolerant of no-space/space bank forms + whitespace artefacts.
//=============================================================================

BOOST_AUTO_TEST_SUITE(ParseAuxId_TestSuite)

BOOST_AUTO_TEST_CASE(AcceptsCanonicalAndVariantSpellings)
{
	// Bank A (no space canonically) - accept with or without a space.
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux1") == Auxillaries::JandyAuxillaryIds::Aux_1);
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux5") == Auxillaries::JandyAuxillaryIds::Aux_5);
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux 5") == Auxillaries::JandyAuxillaryIds::Aux_5);
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux7") == Auxillaries::JandyAuxillaryIds::Aux_7);

	// Banks B-D (space canonically) - accept with, without, or doubled spacing.
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux B1") == Auxillaries::JandyAuxillaryIds::Aux_B1);
	BOOST_CHECK(Auxillaries::ParseAuxId("AuxB1") == Auxillaries::JandyAuxillaryIds::Aux_B1);
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux  B1") == Auxillaries::JandyAuxillaryIds::Aux_B1);
	BOOST_CHECK(Auxillaries::ParseAuxId("  Aux B1  ") == Auxillaries::JandyAuxillaryIds::Aux_B1);
	BOOST_CHECK(Auxillaries::ParseAuxId("Aux D8") == Auxillaries::JandyAuxillaryIds::Aux_D8);
	BOOST_CHECK(Auxillaries::ParseAuxId("AuxD8") == Auxillaries::JandyAuxillaryIds::Aux_D8);

	// Extra Aux, with or without the interior space.
	BOOST_CHECK(Auxillaries::ParseAuxId("Extra Aux") == Auxillaries::JandyAuxillaryIds::ExtraAux);
	BOOST_CHECK(Auxillaries::ParseAuxId("ExtraAux") == Auxillaries::JandyAuxillaryIds::ExtraAux);
}

BOOST_AUTO_TEST_CASE(RejectsNonAuxAndOutOfRange)
{
	BOOST_CHECK(!Auxillaries::ParseAuxId("").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("pool light").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("Pool Light").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("Swim Jet").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("Auxillary").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("Sprinkler").has_value());
	BOOST_CHECK(!Auxillaries::ParseAuxId("Aux11").has_value());      // two digits
	BOOST_CHECK(!Auxillaries::ParseAuxId("Aux0").has_value());       // below range
	BOOST_CHECK(!Auxillaries::ParseAuxId("Aux8").has_value());       // bank A max is 7
	BOOST_CHECK(!Auxillaries::ParseAuxId("Aux E1").has_value());     // no bank E
	BOOST_CHECK(!Auxillaries::ParseAuxId("Aux B9").has_value());     // bank max is 8
	BOOST_CHECK(!Auxillaries::ParseAuxId("Extra Aux 2").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Stable identity - deterministic, collision-free per aux id.
//=============================================================================

BOOST_AUTO_TEST_SUITE(AuxStableId_TestSuite)

BOOST_AUTO_TEST_CASE(NativeKeyIsCanonicalIntegerKeyed)
{
	BOOST_CHECK_EQUAL(Auxillaries::AuxNativeKey(Auxillaries::JandyAuxillaryIds::Aux_5), "jandy:aux:5");
	BOOST_CHECK_EQUAL(Auxillaries::AuxNativeKey(Auxillaries::JandyAuxillaryIds::Aux_B1), "jandy:aux:8");
	BOOST_CHECK_EQUAL(Auxillaries::AuxNativeKey(Auxillaries::JandyAuxillaryIds::ExtraAux), "jandy:aux:0");
}

BOOST_AUTO_TEST_CASE(StableIdIsDeterministicAndDistinct)
{
	const auto a1 = Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_5);
	const auto a2 = Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_5);
	const auto b = Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_6);

	BOOST_CHECK(a1 == a2);                              // same aux id -> same id
	BOOST_CHECK(a1 != b);                               // different aux id -> different id
	BOOST_CHECK(!a1.is_nil());
	// All spelling variants resolve to the SAME stable id (via the same aux id).
	BOOST_CHECK(a1 == Auxillaries::AuxStableId(Auxillaries::ParseAuxId("Aux 5").value()));
	// Equivalent to deriving directly from the native key.
	BOOST_CHECK(a1 == Kernel::DeriveStableUuid("jandy:aux:5"));
}

BOOST_AUTO_TEST_CASE(DeriveStableUuidIsStableAcrossCalls)
{
	BOOST_CHECK(Kernel::DeriveStableUuid("jandy:aux:5") == Kernel::DeriveStableUuid("jandy:aux:5"));
	BOOST_CHECK(Kernel::DeriveStableUuid("jandy:aux:5") != Kernel::DeriveStableUuid("jandy:aux:6"));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Reconciliation helpers - identity adoption + legacy-orphan pruning.
//=============================================================================

namespace
{
	std::shared_ptr<Kernel::AuxillaryDevice> MakeAux(const std::string& label, bool with_aux_id, Auxillaries::JandyAuxillaryIds id)
	{
		auto dev = with_aux_id
			? std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(id))
			: std::make_shared<Kernel::AuxillaryDevice>();
		dev->AuxillaryTraits.Set(Traits::AuxillaryTypeTrait{}, Traits::AuxillaryTypes::Auxillary);
		dev->AuxillaryTraits.Set(Traits::LabelTrait{}, label);
		if (with_aux_id)
		{
			dev->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, id);
		}
		return dev;
	}
}

BOOST_AUTO_TEST_SUITE(AuxReconciliation_TestSuite)

BOOST_AUTO_TEST_CASE(EnsureAuxIdentityGrantsIdentityAndIsIdempotent)
{
	// A cache-restored placeholder: label only, no aux id / hardware label.
	auto phantom = std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(Auxillaries::JandyAuxillaryIds::Aux_5));
	phantom->AuxillaryTraits.Set(Traits::LabelTrait{}, "Swim Jet");

	BOOST_REQUIRE(!phantom->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}));

	Auxillaries::EnsureAuxIdentity(phantom, Auxillaries::JandyAuxillaryIds::Aux_5);

	BOOST_REQUIRE(phantom->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}));
	BOOST_CHECK(*(phantom->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]) == Auxillaries::JandyAuxillaryIds::Aux_5);
	BOOST_REQUIRE(phantom->AuxillaryTraits.Has(Traits::HardwareLabelTrait{}));
	BOOST_CHECK_EQUAL(std::string{ *(phantom->AuxillaryTraits[Traits::HardwareLabelTrait{}]) }, "Aux5");

	// Calling again must NOT throw (HardwareLabelTrait is immutable; the guard prevents a re-set).
	BOOST_CHECK_NO_THROW(Auxillaries::EnsureAuxIdentity(phantom, Auxillaries::JandyAuxillaryIds::Aux_5));
}

BOOST_AUTO_TEST_CASE(RemoveOrphanAuxPlaceholdersDropsTheSharedLabelPlaceholder)
{
	Kernel::DevicesGraph devices;

	// The legacy cache placeholder: type Auxillary, label "Swim Jet", but no aux id.
	auto orphan = MakeAux("Swim Jet", /*with_aux_id=*/false, Auxillaries::JandyAuxillaryIds::Aux_1);
	// The live device starts with its generic label (so Add's type+label dedup lets it in
	// alongside the placeholder), then live discovery relabels it - exactly how the real
	// duplicate arises.
	auto live = MakeAux("Aux1", /*with_aux_id=*/true, Auxillaries::JandyAuxillaryIds::Aux_1);
	auto other = MakeAux("Spa Jet", /*with_aux_id=*/false, Auxillaries::JandyAuxillaryIds::Aux_2);

	devices.Add(orphan);
	devices.Add(live);
	devices.Add(other);
	live->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Swim Jet" });

	// Two "Swim Jet" now exist (placeholder + relabelled live) - the duplicate state.
	BOOST_REQUIRE_EQUAL(devices.CountByLabel("Swim Jet"), 2u);

	Auxillaries::RemoveOrphanAuxPlaceholders(devices, Auxillaries::JandyAuxillaryIds::Aux_1, live);

	// The label-only placeholder is gone; the identified live device is kept.
	BOOST_CHECK_EQUAL(devices.CountByLabel("Swim Jet"), 1u);
	BOOST_CHECK_EQUAL(devices.CountByLabel("Spa Jet"), 1u);   // a different aux is untouched
}

BOOST_AUTO_TEST_CASE(GenericPlaceholderPrunedAtFirstTouchByAuxIdentity)
{
	// The reported scenario: a legacy random-id placeholder labelled with the GENERIC aux name
	// ("Aux5"), no aux identity. At the FIRST live touch the live device still carries only its
	// generic label (the custom label is not enumerated yet) - the placeholder must still be
	// collapsed away, matched by the aux id parsed from its own label.
	Kernel::DevicesGraph devices;

	auto orphan = MakeAux("Aux5", /*with_aux_id=*/false, Auxillaries::JandyAuxillaryIds::Aux_5);
	// Add live under a temp label so Add's type+label dedup admits it, then give it the generic
	// "Aux5" label it carries before the custom label is known.
	auto live = MakeAux("__live__", /*with_aux_id=*/true, Auxillaries::JandyAuxillaryIds::Aux_5);
	devices.Add(orphan);
	devices.Add(live);
	live->AuxillaryTraits.Set(Traits::LabelTrait{}, std::string{ "Aux5" });

	BOOST_REQUIRE_EQUAL(devices.CountByLabel("Aux5"), 2u);

	Auxillaries::RemoveOrphanAuxPlaceholders(devices, Auxillaries::JandyAuxillaryIds::Aux_5, live);

	BOOST_REQUIRE_EQUAL(devices.CountByLabel("Aux5"), 1u);
	// The survivor is the identified live device, not the placeholder.
	BOOST_CHECK(devices.FindByLabel("Aux5").front()->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}));
}

BOOST_AUTO_TEST_CASE(CachedCustomLabelAndBodyTransferredBeforePruning)
{
	// A legacy placeholder that cached a custom label + body + hardware id but with a random id
	// (so it is NOT matched by stable id). When collapsed onto the still-generic live device, the
	// custom label/body must transfer across so the cache-enumerated info is not lost by pruning.
	Kernel::DevicesGraph devices;

	auto orphan = MakeAux("Pool Light", /*with_aux_id=*/false, Auxillaries::JandyAuxillaryIds::Aux_5);
	orphan->AuxillaryTraits.Set(Traits::HardwareLabelTrait{}, std::string{ "Aux5" });
	orphan->AuxillaryTraits.Set(Traits::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Pool);

	auto live = MakeAux("Aux5", /*with_aux_id=*/true, Auxillaries::JandyAuxillaryIds::Aux_5);
	devices.Add(orphan);
	devices.Add(live);

	Auxillaries::RemoveOrphanAuxPlaceholders(devices, Auxillaries::JandyAuxillaryIds::Aux_5, live);

	// Placeholder gone (matched via its hardware id); live now carries the cached custom label + body.
	BOOST_CHECK_EQUAL(devices.CountByLabel("Pool Light"), 1u);
	BOOST_CHECK_EQUAL(devices.CountByLabel("Aux5"), 0u);
	BOOST_REQUIRE(live->AuxillaryTraits.Has(Traits::LabelTrait{}));
	BOOST_CHECK_EQUAL(std::string{ *(live->AuxillaryTraits[Traits::LabelTrait{}]) }, "Pool Light");
	BOOST_REQUIRE(live->AuxillaryTraits.Has(Traits::BodyOfWaterTrait{}));
	BOOST_CHECK(*(live->AuxillaryTraits[Traits::BodyOfWaterTrait{}]) == Kernel::BodyOfWaterIds::Pool);
}

BOOST_AUTO_TEST_CASE(PlaceholderForADifferentAuxIsNotPruned)
{
	// A placeholder belonging to a different aux must never be collapsed onto this one.
	Kernel::DevicesGraph devices;

	auto orphan = MakeAux("Aux6", /*with_aux_id=*/false, Auxillaries::JandyAuxillaryIds::Aux_6);
	auto live = MakeAux("Aux5", /*with_aux_id=*/true, Auxillaries::JandyAuxillaryIds::Aux_5);
	devices.Add(orphan);
	devices.Add(live);

	Auxillaries::RemoveOrphanAuxPlaceholders(devices, Auxillaries::JandyAuxillaryIds::Aux_5, live);

	BOOST_CHECK_EQUAL(devices.CountByLabel("Aux6"), 1u);   // different aux: untouched
	BOOST_CHECK_EQUAL(devices.CountByLabel("Aux5"), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// enum_name customization + native-key / stable-id across EVERY aux value.
// Exercises all 32 arms of the magic_enum enum_name<JandyAuxillaryIds> switch
// (display names) and the per-id native key + stable UUID, and proves the
// display name round-trips back through ParseAuxId to the same enum.
//=============================================================================

BOOST_AUTO_TEST_SUITE(JandyAuxillaryId_AllValues_TestSuite)

BOOST_AUTO_TEST_CASE(EnumName_RoundTrips_And_KeysAreConsistent_ForEveryAux)
{
	using Auxillaries::JandyAuxillaryIds;

	// {enum value, expected display name, expected native-key integer}.
	const std::vector<std::tuple<JandyAuxillaryIds, std::string_view, int>> all = {
		{ JandyAuxillaryIds::Aux_1, "Aux1", 1 },   { JandyAuxillaryIds::Aux_2, "Aux2", 2 },
		{ JandyAuxillaryIds::Aux_3, "Aux3", 3 },   { JandyAuxillaryIds::Aux_4, "Aux4", 4 },
		{ JandyAuxillaryIds::Aux_5, "Aux5", 5 },   { JandyAuxillaryIds::Aux_6, "Aux6", 6 },
		{ JandyAuxillaryIds::Aux_7, "Aux7", 7 },
		{ JandyAuxillaryIds::Aux_B1, "Aux B1", 8 },  { JandyAuxillaryIds::Aux_B2, "Aux B2", 9 },
		{ JandyAuxillaryIds::Aux_B3, "Aux B3", 10 }, { JandyAuxillaryIds::Aux_B4, "Aux B4", 11 },
		{ JandyAuxillaryIds::Aux_B5, "Aux B5", 12 }, { JandyAuxillaryIds::Aux_B6, "Aux B6", 13 },
		{ JandyAuxillaryIds::Aux_B7, "Aux B7", 14 }, { JandyAuxillaryIds::Aux_B8, "Aux B8", 15 },
		{ JandyAuxillaryIds::Aux_C1, "Aux C1", 16 }, { JandyAuxillaryIds::Aux_C2, "Aux C2", 17 },
		{ JandyAuxillaryIds::Aux_C3, "Aux C3", 18 }, { JandyAuxillaryIds::Aux_C4, "Aux C4", 19 },
		{ JandyAuxillaryIds::Aux_C5, "Aux C5", 20 }, { JandyAuxillaryIds::Aux_C6, "Aux C6", 21 },
		{ JandyAuxillaryIds::Aux_C7, "Aux C7", 22 }, { JandyAuxillaryIds::Aux_C8, "Aux C8", 23 },
		{ JandyAuxillaryIds::Aux_D1, "Aux D1", 24 }, { JandyAuxillaryIds::Aux_D2, "Aux D2", 25 },
		{ JandyAuxillaryIds::Aux_D3, "Aux D3", 26 }, { JandyAuxillaryIds::Aux_D4, "Aux D4", 27 },
		{ JandyAuxillaryIds::Aux_D5, "Aux D5", 28 }, { JandyAuxillaryIds::Aux_D6, "Aux D6", 29 },
		{ JandyAuxillaryIds::Aux_D7, "Aux D7", 30 }, { JandyAuxillaryIds::Aux_D8, "Aux D8", 31 },
		{ JandyAuxillaryIds::ExtraAux, "Extra Aux", 0 },
	};

	BOOST_REQUIRE_EQUAL(all.size(), 32u);   // every enumerator is exercised

	for (const auto& [id, expected_name, key_int] : all)
	{
		// enum_name customization (runtime lookup drives the switch arm for this value).
		const std::string name{ magic_enum::enum_name(id) };
		BOOST_CHECK_EQUAL(name, std::string(expected_name));

		// Native key is the integer-keyed protocol identity.
		BOOST_CHECK_EQUAL(Auxillaries::AuxNativeKey(id), "jandy:aux:" + std::to_string(key_int));

		// Stable UUID is deterministic and non-nil for every aux.
		const auto uuid = Auxillaries::AuxStableId(id);
		BOOST_CHECK(!uuid.is_nil());
		BOOST_CHECK(uuid == Auxillaries::AuxStableId(id));

		// The display name parses back to the same enum (the label contract).
		BOOST_CHECK(Auxillaries::ParseAuxId(expected_name) == id);
	}
}

BOOST_AUTO_TEST_SUITE_END()
