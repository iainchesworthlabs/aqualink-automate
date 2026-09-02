#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_presence_override.h"
#include "jandy/auxillaries/jandy_auxillary_reconciliation.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/device_graph/device_graph.h"

using namespace AqualinkAutomate;
namespace Traits = Kernel::AuxillaryTraitsTypes;

//=============================================================================
// AuxPresenceOverride - operator override of live aux presence detection.
// See jandy_auxillary_presence_override.h for the concept: "Present"/"Absent"
// overrules whatever detection concludes for a slot; "Auto" (the default, no
// entry in the overrides map) leaves it to detection.
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
}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(GetPresenceOverride_TestSuite)

BOOST_AUTO_TEST_CASE(MissingEntryIsAuto)
{
	const nlohmann::json overrides = nlohmann::json::object();
	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Auto == Auxillaries::GetPresenceOverride(Aux_5, overrides));
	BOOST_CHECK(!Auxillaries::IsForcedAbsent(Aux_5, overrides));
	BOOST_CHECK(!Auxillaries::IsForcedPresent(Aux_5, overrides));
}

BOOST_AUTO_TEST_CASE(RecognisesPresentAndAbsent)
{
	const nlohmann::json overrides = { { "Aux5", "present" }, { "Aux6", "absent" } };

	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Present == Auxillaries::GetPresenceOverride(Aux_5, overrides));
	BOOST_CHECK(Auxillaries::IsForcedPresent(Aux_5, overrides));
	BOOST_CHECK(!Auxillaries::IsForcedAbsent(Aux_5, overrides));

	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Absent == Auxillaries::GetPresenceOverride(Aux_6, overrides));
	BOOST_CHECK(Auxillaries::IsForcedAbsent(Aux_6, overrides));
	BOOST_CHECK(!Auxillaries::IsForcedPresent(Aux_6, overrides));

	// Not mentioned at all -- Auto.
	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Auto == Auxillaries::GetPresenceOverride(Aux_7, overrides));
}

BOOST_AUTO_TEST_CASE(UnrecognisedOrWrongTypeValueIsAuto)
{
	// An override is only ever an explicit operator choice -- anything malformed must not be
	// silently treated as a real Present/Absent decision.
	const nlohmann::json bad_string = { { "Aux5", "forced-on" } };
	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Auto == Auxillaries::GetPresenceOverride(Aux_5, bad_string));

	const nlohmann::json wrong_type = { { "Aux5", true } };
	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Auto == Auxillaries::GetPresenceOverride(Aux_5, wrong_type));

	const nlohmann::json not_an_object = nlohmann::json::array();
	BOOST_CHECK(Auxillaries::AuxPresenceOverride::Auto == Auxillaries::GetPresenceOverride(Aux_5, not_an_object));
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// ApplyPresenceOverrides - reconciles the device graph against the overrides.
//=============================================================================

BOOST_AUTO_TEST_SUITE(ApplyPresenceOverrides_TestSuite)

BOOST_AUTO_TEST_CASE(PresentOverrideSynthesizesADeviceWithNoWireEvidence)
{
	// The core "kept not detecting Aux5" scenario: nothing has ever created a device for it, but
	// the operator knows it is really there.
	Kernel::DevicesGraph devices;
	const nlohmann::json overrides = { { "Aux5", "present" } };

	BOOST_REQUIRE(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_5)));

	const auto changed = Auxillaries::ApplyPresenceOverrides(devices, overrides);

	BOOST_CHECK_EQUAL(changed, 1u);
	const auto device = devices.FindById(Auxillaries::AuxStableId(Aux_5));
	BOOST_REQUIRE(nullptr != device);
	BOOST_CHECK(device->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}));
	BOOST_CHECK(Aux_5 == *(device->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]));
}

BOOST_AUTO_TEST_CASE(PresentOverrideIsANoOpWhenAlreadyDetected)
{
	Kernel::DevicesGraph devices;
	auto existing = AddAux(devices, Aux_5, "Pool Light");
	const nlohmann::json overrides = { { "Aux5", "present" } };

	const auto changed = Auxillaries::ApplyPresenceOverrides(devices, overrides);

	BOOST_CHECK_EQUAL(changed, 0u);
	// The SAME device survives -- a real device must never be replaced by a synthesized one.
	BOOST_CHECK(existing == devices.FindById(Auxillaries::AuxStableId(Aux_5)));
}

BOOST_AUTO_TEST_CASE(AbsentOverrideRemovesAnExistingDevice)
{
	Kernel::DevicesGraph devices;
	AddAux(devices, Aux_6, "Aux6");
	const nlohmann::json overrides = { { "Aux6", "absent" } };

	const auto changed = Auxillaries::ApplyPresenceOverrides(devices, overrides);

	BOOST_CHECK_EQUAL(changed, 1u);
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_6)));
}

BOOST_AUTO_TEST_CASE(AbsentOverrideIsANoOpWhenNothingExists)
{
	Kernel::DevicesGraph devices;
	const nlohmann::json overrides = { { "Aux6", "absent" } };

	BOOST_CHECK_EQUAL(Auxillaries::ApplyPresenceOverrides(devices, overrides), 0u);
}

BOOST_AUTO_TEST_CASE(AutoAndMalformedEntriesAreIgnored)
{
	Kernel::DevicesGraph devices;
	AddAux(devices, Aux_1, "Spa Jets");
	const nlohmann::json overrides = { { "Aux1", "auto" }, { "Not An Aux Id", "present" } };

	BOOST_CHECK_EQUAL(Auxillaries::ApplyPresenceOverrides(devices, overrides), 0u);
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_1)));
}

BOOST_AUTO_TEST_CASE(HandlesMultipleOverridesInOnePass)
{
	Kernel::DevicesGraph devices;
	AddAux(devices, Aux_2, "Waterfall");
	const nlohmann::json overrides = { { "Aux2", "absent" }, { "Aux5", "present" } };

	const auto changed = Auxillaries::ApplyPresenceOverrides(devices, overrides);

	BOOST_CHECK_EQUAL(changed, 2u);
	BOOST_CHECK(nullptr == devices.FindById(Auxillaries::AuxStableId(Aux_2)));
	BOOST_CHECK(nullptr != devices.FindById(Auxillaries::AuxStableId(Aux_5)));
}

BOOST_AUTO_TEST_SUITE_END()
