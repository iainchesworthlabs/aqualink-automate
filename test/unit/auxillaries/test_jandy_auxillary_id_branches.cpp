#include <string>
#include <string_view>

#include <boost/test/unit_test.hpp>
#include <magic_enum/magic_enum.hpp>

#include "jandy/auxillaries/jandy_auxillary_id.h"

using namespace AqualinkAutomate;

//=============================================================================
// The magic_enum NAME CUSTOMISATION for JandyAuxillaryIds, exercised as a
// runtime function rather than through magic_enum's compile-time name table.
//
// These names are not cosmetic: GetPresenceOverride keys the operator's
// aux presence overrides by magic_enum::enum_name(id) ("Aux5", "Aux B1"), the
// screen-scraped hardware labels are parsed back with ParseAuxId, and both are
// persisted. A silent change to one arm of the customisation would orphan a
// stored override without any other test noticing.
//
// Calling the customisation point directly (with a runtime value) is what makes
// each arm an executed decision instead of a constant folded into the table.
//=============================================================================

BOOST_AUTO_TEST_SUITE(JandyAuxillaryIdCustomisation_TestSuite)

BOOST_AUTO_TEST_CASE(EveryAuxId_HasACustomNameThatMatchesWhatMagicEnumPublishes)
{
	using Auxillaries::JandyAuxillaryIds;

	const auto values = magic_enum::enum_values<JandyAuxillaryIds>();
	BOOST_REQUIRE_EQUAL(values.size(), 32u);   // 7 bank-A + 24 banks B-D + Extra Aux

	for (const auto id : values)
	{
		BOOST_TEST_CONTEXT(static_cast<int>(magic_enum::enum_integer(id)))
		{
			// The customisation point, invoked with a RUNTIME value.
			const auto customised = magic_enum::customize::enum_name<JandyAuxillaryIds>(id);

			// Every enumerator is customised (none falls through to magic_enum's raw
			// identifier, which would spell "Aux_1" / "Aux_B1" instead of "Aux1" / "Aux B1").
			BOOST_REQUIRE(!customised.second.empty());

			// ...and what it returns is exactly what magic_enum publishes for that value.
			const std::string published{ magic_enum::enum_name(id) };
			BOOST_CHECK_EQUAL(std::string(customised.second), published);

			// The published name is the persisted key contract: it round-trips through the
			// label parser back to the same enumerator.
			BOOST_CHECK(Auxillaries::ParseAuxId(published) == id);
		}
	}
}

BOOST_AUTO_TEST_CASE(BankNamingConvention_IsPreservedByTheCustomisation)
{
	using Auxillaries::JandyAuxillaryIds;

	// Bank A is spelled WITHOUT a space; banks B-D and Extra Aux WITH one. Both spellings are
	// load-bearing (they are the override keys and the scraped hardware labels).
	const auto name_of = [](JandyAuxillaryIds id)
	{
		return std::string(magic_enum::customize::enum_name<JandyAuxillaryIds>(id).second);
	};

	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::Aux_1), std::string("Aux1"));
	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::Aux_7), std::string("Aux7"));
	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::Aux_B1), std::string("Aux B1"));
	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::Aux_C8), std::string("Aux C8"));
	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::Aux_D8), std::string("Aux D8"));
	BOOST_CHECK_EQUAL(name_of(JandyAuxillaryIds::ExtraAux), std::string("Extra Aux"));
}

BOOST_AUTO_TEST_CASE(AValueOutsideTheEnum_FallsBackToTheDefaultNaming)
{
	using Auxillaries::JandyAuxillaryIds;

	// A wire byte that is not a known relay must not be given a fabricated name: the
	// customisation defers to magic_enum's default handling, which yields no name at all.
	const auto not_an_aux = static_cast<JandyAuxillaryIds>(0x77);
	const auto customised = magic_enum::customize::enum_name<JandyAuxillaryIds>(not_an_aux);

	BOOST_CHECK(customised.second.empty());
	BOOST_CHECK(!magic_enum::enum_cast<JandyAuxillaryIds>(0x77).has_value());
	BOOST_CHECK(magic_enum::enum_name(not_an_aux).empty());
}

BOOST_AUTO_TEST_SUITE_END()
