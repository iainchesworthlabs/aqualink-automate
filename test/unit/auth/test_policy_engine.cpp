#include <boost/test/unit_test.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/group.h"
#include "auth/policy_engine.h"
#include "auth/subject.h"

using namespace AqualinkAutomate;

namespace
{
	Auth::Subject MakeSubject(std::initializer_list<const char*> entitlements)
	{
		Auth::Subject subject;
		subject.Id = "test-user";
		subject.Authenticated = true;
		subject.Provider = Auth::SubjectProvider::Local;

		for (const auto* text : entitlements)
		{
			subject.Entitlements.Add(*Auth::Entitlement::Parse(text));
		}

		return subject;
	}

	const Auth::Environment AUTH_ON{ .AuthEnabled = true };
	const Auth::Environment AUTH_OFF{ .AuthEnabled = false };
}

BOOST_AUTO_TEST_SUITE(TestSuite_PolicyEngine)

//-----------------------------------------------------------------------------
// POSTURE (environment condition)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_AuthOff_PermitsEverythingForAnonymous)
{
	const auto anonymous = Auth::Subject::Anonymous();

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(anonymous, "equipment.view", {}, AUTH_OFF));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(anonymous, "equipment.control.aux", { .Kind = "aux", .Id = "AUX3" }, AUTH_OFF));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(anonymous, "system.admin", {}, AUTH_OFF));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_AuthOn_DefaultDeny)
{
	const auto anonymous = Auth::Subject::Anonymous();

	// Anonymous with no Guest grants: deny-by-default across the board.
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(anonymous, "equipment.view", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(anonymous, "equipment.control.aux", { .Kind = "aux", .Id = "AUX3" }, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(anonymous, "system.admin", {}, AUTH_ON));
}

//-----------------------------------------------------------------------------
// ENTITLEMENT MATCHING
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_PermitsExactEntitlement)
{
	const auto subject = MakeSubject({ "equipment.view" });

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.view", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(subject, "equipment.control.heater", {}, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_PerResourceSelectors)
{
	const auto subject = MakeSubject({ "equipment.control.aux:AUX3" });

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.control.aux", { .Kind = "aux", .Id = "AUX3" }, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(subject, "equipment.control.aux", { .Kind = "aux", .Id = "AUX4" }, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_WildcardSelector)
{
	const auto subject = MakeSubject({ "equipment.control.aux:*" });

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.control.aux", { .Kind = "aux", .Id = "AUX3" }, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.control.aux", { .Kind = "aux", .Id = "AUX9" }, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_SystemAdminIsSuperuser)
{
	const auto admin = MakeSubject({ "system.admin" });

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(admin, "equipment.view", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(admin, "equipment.control.aux", { .Kind = "aux", .Id = "AUX3" }, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(admin, "schedules.edit", {}, AUTH_ON));
}

//-----------------------------------------------------------------------------
// GROUP RESOLUTION -> DECISIONS (person -> group -> entitlements)
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_GroupDerivedEntitlements)
{
	auto registry = Auth::GroupRegistry::WithBuiltIns();

	// Admin scopes the Guest group: view + one specific aux.
	Auth::Group guest{ .Name = std::string{ Auth::BuiltInGroups::GUEST }, .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:AUX5" }), .BuiltIn = true };
	registry.Upsert(guest);

	Auth::Subject anonymous = Auth::Subject::Anonymous();
	anonymous.Groups = { std::string{ Auth::BuiltInGroups::GUEST } };
	anonymous.Entitlements = registry.ResolveEffectiveEntitlements({}, anonymous.Groups);

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(anonymous, "equipment.view", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(anonymous, "equipment.control.aux", { .Kind = "aux", .Id = "AUX5" }, AUTH_ON));

	// Un-granted aux and un-granted control types remain denied.
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(anonymous, "equipment.control.aux", { .Kind = "aux", .Id = "AUX1" }, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(anonymous, "equipment.control.heater", {}, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_DirectGrantsUnionWithGroups)
{
	const auto registry = Auth::GroupRegistry::WithBuiltIns();

	Auth::Subject subject;
	subject.Id = "user-1";
	subject.Authenticated = true;
	subject.Groups = {}; // No group membership at all.
	subject.Entitlements = registry.ResolveEffectiveEntitlements(Auth::EntitlementSet::Parse({ "equipment.control.heater" }), subject.Groups);

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.control.heater", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(subject, "equipment.view", {}, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_EveryoneGroupAppliesToAllSubjects)
{
	auto registry = Auth::GroupRegistry::WithBuiltIns();

	Auth::Group everyone{ .Name = std::string{ Auth::BuiltInGroups::EVERYONE }, .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" }), .BuiltIn = true };
	registry.Upsert(everyone);

	// Even a subject with no memberships receives Everyone's entitlements.
	Auth::Subject subject;
	subject.Entitlements = registry.ResolveEffectiveEntitlements({}, {});

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.view", {}, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_AdministratorsBuiltInIsSuperuser)
{
	const auto registry = Auth::GroupRegistry::WithBuiltIns();

	Auth::Subject subject;
	subject.Id = "admin-user";
	subject.Authenticated = true;
	subject.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
	subject.Entitlements = registry.ResolveEffectiveEntitlements({}, subject.Groups);

	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "equipment.control.chlorinator", {}, AUTH_ON));
	BOOST_CHECK(Auth::Decision::Permit == Auth::PolicyEngine::Decide(subject, "system.admin", {}, AUTH_ON));
}

BOOST_AUTO_TEST_CASE(Test_PolicyEngine_UnknownGroupMembershipDegradesSafely)
{
	const auto registry = Auth::GroupRegistry::WithBuiltIns();

	Auth::Subject subject;
	subject.Groups = { "DeletedGroup" };
	subject.Entitlements = registry.ResolveEffectiveEntitlements({}, subject.Groups);

	BOOST_CHECK(Auth::Decision::Deny == Auth::PolicyEngine::Decide(subject, "equipment.view", {}, AUTH_ON));
}

BOOST_AUTO_TEST_SUITE_END()
