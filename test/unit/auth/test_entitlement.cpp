#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

#include "auth/entitlement.h"
#include "auth/entitlement_vocabulary.h"

using namespace AqualinkAutomate;

BOOST_AUTO_TEST_SUITE(TestSuite_Entitlement)

//-----------------------------------------------------------------------------
// PARSING
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_Entitlement_Parse_PlainAction)
{
	const auto ent = Auth::Entitlement::Parse("equipment.view");

	BOOST_REQUIRE(ent.has_value());
	BOOST_CHECK_EQUAL(ent->Action(), "equipment.view");
	BOOST_CHECK(!ent->Selector().has_value());
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Parse_ActionWithWildcardSelector)
{
	const auto ent = Auth::Entitlement::Parse("equipment.control.aux:*");

	BOOST_REQUIRE(ent.has_value());
	BOOST_CHECK_EQUAL(ent->Action(), "equipment.control.aux");
	BOOST_REQUIRE(ent->Selector().has_value());
	BOOST_CHECK_EQUAL(*(ent->Selector()), "*");
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Parse_ActionWithResourceSelector)
{
	const auto ent = Auth::Entitlement::Parse("equipment.control.aux:5e17c9b2-0001-4a2b-9c1d-70e6a1b2c3d4");

	BOOST_REQUIRE(ent.has_value());
	BOOST_CHECK_EQUAL(ent->Action(), "equipment.control.aux");
	BOOST_REQUIRE(ent->Selector().has_value());
	BOOST_CHECK_EQUAL(*(ent->Selector()), "5e17c9b2-0001-4a2b-9c1d-70e6a1b2c3d4");
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Parse_RejectsMalformedInput)
{
	// No dot (single segment).
	BOOST_CHECK(!Auth::Entitlement::Parse("admin").has_value());

	// Empty / whitespace / bad characters.
	BOOST_CHECK(!Auth::Entitlement::Parse("").has_value());
	BOOST_CHECK(!Auth::Entitlement::Parse("equipment view").has_value());
	BOOST_CHECK(!Auth::Entitlement::Parse("Equipment.View").has_value());

	// Degenerate dots.
	BOOST_CHECK(!Auth::Entitlement::Parse(".view").has_value());
	BOOST_CHECK(!Auth::Entitlement::Parse("equipment.").has_value());
	BOOST_CHECK(!Auth::Entitlement::Parse("equipment..view").has_value());

	// Colon with no selector after it.
	BOOST_CHECK(!Auth::Entitlement::Parse("equipment.control.aux:").has_value());
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Parse_RoundTripsThroughToString)
{
	for (const auto text : { "equipment.view", "equipment.control.aux:*", "equipment.control.aux:AUX3" })
	{
		const auto ent = Auth::Entitlement::Parse(text);
		BOOST_REQUIRE(ent.has_value());
		BOOST_CHECK_EQUAL(ent->ToString(), text);
	}
}

//-----------------------------------------------------------------------------
// MATCHING (selector semantics)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_Entitlement_Match_ExactActionOnly)
{
	const auto ent = *Auth::Entitlement::Parse("equipment.view");

	BOOST_CHECK(ent.Matches("equipment.view"));

	// No hierarchy is implied in either direction.
	BOOST_CHECK(!ent.Matches("equipment"));
	BOOST_CHECK(!ent.Matches("equipment.view.detailed"));
	BOOST_CHECK(!ent.Matches("equipment.control.heater"));
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Match_SelectorlessRequiresResourcelessRequest)
{
	const auto ent = *Auth::Entitlement::Parse("equipment.control.aux");

	BOOST_CHECK(ent.Matches("equipment.control.aux", ""));

	// A selector-less grant does NOT reach specific resources; "*" must be
	// used explicitly (deny-by-default posture of the design).
	BOOST_CHECK(!ent.Matches("equipment.control.aux", "AUX3"));
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Match_WildcardSelectorMatchesAnyResource)
{
	const auto ent = *Auth::Entitlement::Parse("equipment.control.aux:*");

	BOOST_CHECK(ent.Matches("equipment.control.aux", "AUX3"));
	BOOST_CHECK(ent.Matches("equipment.control.aux", "anything"));
	BOOST_CHECK(ent.Matches("equipment.control.aux", ""));

	BOOST_CHECK(!ent.Matches("equipment.control.heater", "AUX3"));
}

BOOST_AUTO_TEST_CASE(Test_Entitlement_Match_SpecificSelectorMatchesOnlyThatResource)
{
	const auto ent = *Auth::Entitlement::Parse("equipment.control.aux:AUX3");

	BOOST_CHECK(ent.Matches("equipment.control.aux", "AUX3"));

	BOOST_CHECK(!ent.Matches("equipment.control.aux", "AUX4"));
	BOOST_CHECK(!ent.Matches("equipment.control.aux", "aux3")); // Case-sensitive.
	BOOST_CHECK(!ent.Matches("equipment.control.aux", ""));
}

//-----------------------------------------------------------------------------
// ENTITLEMENT SET
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_EntitlementSet_Parse_SkipsAndReportsMalformedEntries)
{
	std::vector<std::string> rejected;

	const auto set = Auth::EntitlementSet::Parse({ "equipment.view", "bogus", "equipment.control.aux:*" }, &rejected);

	BOOST_CHECK_EQUAL(set.Size(), 2u);
	BOOST_REQUIRE_EQUAL(rejected.size(), 1u);
	BOOST_CHECK_EQUAL(rejected[0], "bogus");
}

BOOST_AUTO_TEST_CASE(Test_EntitlementSet_Add_Deduplicates)
{
	Auth::EntitlementSet set;

	set.Add(*Auth::Entitlement::Parse("equipment.view"));
	set.Add(*Auth::Entitlement::Parse("equipment.view"));

	BOOST_CHECK_EQUAL(set.Size(), 1u);
}

BOOST_AUTO_TEST_CASE(Test_EntitlementSet_Permits_AnyMemberSatisfies)
{
	const auto set = Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:AUX3" });

	BOOST_CHECK(set.Permits("equipment.view"));
	BOOST_CHECK(set.Permits("equipment.control.aux", "AUX3"));

	BOOST_CHECK(!set.Permits("equipment.control.aux", "AUX4"));
	BOOST_CHECK(!set.Permits("equipment.control.heater"));
	BOOST_CHECK(!set.Permits("system.admin"));
}

BOOST_AUTO_TEST_CASE(Test_EntitlementSet_ToStrings_IsSortedAndDeterministic)
{
	const auto set = Auth::EntitlementSet::Parse({ "schedules.view", "equipment.view", "equipment.control.aux:*" });

	const auto strings = set.ToStrings();

	BOOST_REQUIRE_EQUAL(strings.size(), 3u);
	BOOST_CHECK_EQUAL(strings[0], "equipment.control.aux:*");
	BOOST_CHECK_EQUAL(strings[1], "equipment.view");
	BOOST_CHECK_EQUAL(strings[2], "schedules.view");
}

BOOST_AUTO_TEST_CASE(Test_EntitlementSet_Merge_IsUnion)
{
	auto lhs = Auth::EntitlementSet::Parse({ "equipment.view" });
	const auto rhs = Auth::EntitlementSet::Parse({ "equipment.view", "schedules.view" });

	lhs.Merge(rhs);

	BOOST_CHECK_EQUAL(lhs.Size(), 2u);
	BOOST_CHECK(lhs.Permits("schedules.view"));
}

//-----------------------------------------------------------------------------
// VOCABULARY
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_Vocabulary_KnownActionsParseAndAreKnown)
{
	for (const auto action : Auth::Vocabulary::ALL_ACTIONS)
	{
		BOOST_CHECK_MESSAGE(Auth::Entitlement::Parse(action).has_value(), std::string{ action } + " must parse");
		BOOST_CHECK(Auth::Vocabulary::IsKnownAction(action));
	}

	BOOST_CHECK(!Auth::Vocabulary::IsKnownAction("equipment.control.lasers"));
}

BOOST_AUTO_TEST_SUITE_END()
