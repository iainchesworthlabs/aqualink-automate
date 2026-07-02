#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>

#include "auth/api_key_store.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/server_types.h"

using namespace AqualinkAutomate;

namespace
{
	namespace fs = std::filesystem;

	struct ResolverFixture
	{
		ResolverFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-resolver-test-{}", counter++);
			fs::create_directories(Dir);

			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));
			GroupsStore = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			ApiKeys = std::make_shared<Auth::ApiKeyStore>(Auth::ApiKeyStore::Load(Dir / "api-keys.json"));

			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(keys, Auth::JwtCodec::Config{});

			Resolver = Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = GroupsStore->SharedRegistry(),
				.Codec = Codec,
				.Users = Users,
				.ApiKeys = ApiKeys });

			// One enabled user in the Administrators group.
			std::string error;
			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = "$argon2id$fake";
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE_MESSAGE(Users->Create(std::move(alice), error), error);

			AliceId = Users->FindByUsername("alice")->Id;
		}

		~ResolverFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		// Mint an access token exactly as the session service does.
		std::string MintFor(const Auth::UserRecord& user, std::size_t ent_budget = 2048)
		{
			auto keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			const Auth::JwtCodec codec(keys, Auth::JwtCodec::Config{ .EntClaimBudgetBytes = ent_budget });

			Auth::TokenClaims claims;
			claims.Subject = user.Id;
			claims.Provider = Auth::SubjectProvider::Local;
			claims.TokenVersion = user.TokenVersion;
			claims.Groups = user.Groups;
			claims.Entitlements = GroupsStore->Registry().ResolveEffectiveEntitlements(user.DirectEntitlements, user.Groups).ToStrings();

			return codec.Sign(claims);
		}

		Auth::Subject Resolve(std::string_view bearer = {})
		{
			HTTP::Request req;
			req.version(11);
			req.method(boost::beast::http::verb::get);
			req.target("/api/equipment");

			if (!bearer.empty())
			{
				req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ bearer });
			}

			return Resolver(req, false);
		}

		fs::path Dir;
		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::GroupStore> GroupsStore;
		std::shared_ptr<Auth::ApiKeyStore> ApiKeys;
		std::shared_ptr<Auth::JwtCodec> Codec;
		HTTP::Routing::SubjectResolver Resolver;
		std::string AliceId;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_SubjectResolver)

BOOST_FIXTURE_TEST_CASE(Test_Resolver_NoCredentialIsGuest, ResolverFixture)
{
	const auto subject = Resolve();

	BOOST_CHECK(!subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::Anonymous == subject.Provider);
	BOOST_CHECK(subject.Entitlements.Empty());  // Guest starts deny-by-default.

	// Guest scoping via the LIVE shared registry applies with no re-wiring.
	std::string error;
	Auth::Group guest{ .Name = std::string{ Auth::BuiltInGroups::GUEST }, .Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" }) };
	BOOST_REQUIRE(GroupsStore->Upsert(std::move(guest), error));

	BOOST_CHECK(Resolve().Entitlements.Permits("equipment.view"));
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_ValidJwtIsAuthenticated, ResolverFixture)
{
	const auto subject = Resolve(MintFor(*Users->FindById(AliceId)));

	BOOST_CHECK(subject.Authenticated);
	BOOST_CHECK_EQUAL(subject.Id, AliceId);
	BOOST_CHECK(Auth::SubjectProvider::Local == subject.Provider);
	BOOST_CHECK(subject.Entitlements.Permits("system.admin"));
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_TokverBumpRevokesOutstandingTokens, ResolverFixture)
{
	const auto token = MintFor(*Users->FindById(AliceId));

	BOOST_CHECK(Resolve(token).Authenticated);

	// D15: logout-all / password change / entitlement change bumps tokver...
	Users->BumpTokenVersion(AliceId);

	// ...and the still-validly-signed token is now stale => anonymous.
	BOOST_CHECK(!Resolve(token).Authenticated);
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_DisabledUserTokenRejected, ResolverFixture)
{
	const auto token = MintFor(*Users->FindById(AliceId));

	// Disable directly at the store (bypassing last-admin protection is fine
	// here: Update() refuses, so flip via a second admin path — simplest is a
	// second admin then disable).
	std::string error;
	Auth::UserRecord bob;
	bob.Username = "bob";
	bob.PasswordHash = "$argon2id$fake";
	bob.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
	BOOST_REQUIRE(Users->Create(std::move(bob), error));

	auto alice = *Users->FindById(AliceId);
	alice.Disabled = true;
	BOOST_REQUIRE_MESSAGE(Users->Update(alice, GroupsStore->Registry(), error), error);

	BOOST_CHECK(!Resolve(token).Authenticated);
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_EntOverflowReResolvesFromStore, ResolverFixture)
{
	// Budget of 1 byte forces the elided-ent path: grp travels, ent does not.
	const auto token = MintFor(*Users->FindById(AliceId), 1);

	const auto subject = Resolve(token);

	BOOST_CHECK(subject.Authenticated);
	BOOST_CHECK(subject.Entitlements.Permits("system.admin"));  // Re-resolved via Administrators.
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_ApiKeyAuthenticatesAsMachineSubject, ResolverFixture)
{
	std::string key_id, error;
	const auto secret = ApiKeys->Create("ha-bridge", Auth::EntitlementSet::Parse({ "equipment.view", "equipment.control.aux:*" }), 0, key_id);

	const auto subject = Resolve(secret);

	BOOST_CHECK(subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::ApiKey == subject.Provider);
	BOOST_CHECK_EQUAL(subject.Id, key_id);
	BOOST_CHECK(subject.Entitlements.Permits("equipment.control.aux", "AUX2"));
	BOOST_CHECK(!subject.Entitlements.Permits("system.admin"));

	// Revocation applies on the next request.
	BOOST_REQUIRE(ApiKeys->Revoke(key_id, error));
	BOOST_CHECK(!Resolve(secret).Authenticated);
}

BOOST_FIXTURE_TEST_CASE(Test_Resolver_LegacyTokenFoldsInAsBootstrapAdmin, ResolverFixture)
{
	ApiKeys->SeedBootstrapKey("operator-legacy-token-value");

	const auto subject = Resolve("operator-legacy-token-value");

	BOOST_CHECK(subject.Authenticated);
	BOOST_CHECK(Auth::SubjectProvider::ApiKey == subject.Provider);
	BOOST_CHECK(subject.Entitlements.Permits("system.admin"));

	// Garbage still degrades to guest.
	BOOST_CHECK(!Resolve("not-a-real-credential").Authenticated);
}

BOOST_AUTO_TEST_SUITE_END()
