#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <nlohmann/json.hpp>

#include "http/server/server_types.h"

#include "mocks/mock_oidc_provider.h"
#include "mocks/mock_trusted_proxy.h"

using namespace AqualinkAutomate;
using namespace std::chrono_literals;

namespace
{
	using JwtTraits = jwt::traits::nlohmann_json;

	// Rebuild the public-key PEM from the JWKS "n"/"e" fields (RFC 7518 §6.3)
	// so tokens are verified against the key AS ADVERTISED — proving the JWKS
	// encoding itself, not just a PEM smuggled out of the provider.
	std::string PemFromJwks(const nlohmann::json& jwks)
	{
		const auto& jwk = jwks.at("keys").at(0);

		return jwt::helper::create_public_key_from_rsa_components(
			jwk.at("n").get<std::string>(),
			jwk.at("e").get<std::string>());
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_MockIdp)

//-----------------------------------------------------------------------------
// DISCOVERY + JWKS SHAPE
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MockIdp_DiscoveryDocumentShape)
{
	const Test::MockOidcProvider idp;

	const auto discovery = idp.DiscoveryDocument();

	BOOST_CHECK_EQUAL(discovery.at("issuer").get<std::string>(), "https://mock-idp.test");
	BOOST_CHECK_EQUAL(discovery.at("authorization_endpoint").get<std::string>(), "https://mock-idp.test/authorize");
	BOOST_CHECK_EQUAL(discovery.at("token_endpoint").get<std::string>(), "https://mock-idp.test/token");
	BOOST_CHECK_EQUAL(discovery.at("jwks_uri").get<std::string>(), "https://mock-idp.test/jwks");
}

BOOST_AUTO_TEST_CASE(Test_MockIdp_DiscoveryDocumentHonoursConfiguredIssuer)
{
	const Test::MockOidcProvider idp(Test::MockOidcProvider::Config{ .Issuer = "https://idp.example.org" });

	const auto discovery = idp.DiscoveryDocument();

	BOOST_CHECK_EQUAL(discovery.at("issuer").get<std::string>(), "https://idp.example.org");
	BOOST_CHECK_EQUAL(discovery.at("jwks_uri").get<std::string>(), "https://idp.example.org/jwks");
}

BOOST_AUTO_TEST_CASE(Test_MockIdp_JwksAdvertisesOneRsaSigningKey)
{
	const Test::MockOidcProvider idp;

	const auto jwks = idp.Jwks();

	BOOST_REQUIRE(jwks.contains("keys"));
	BOOST_REQUIRE_EQUAL(jwks.at("keys").size(), 1u);

	const auto& jwk = jwks.at("keys").at(0);

	BOOST_CHECK_EQUAL(jwk.at("kty").get<std::string>(), "RSA");
	BOOST_CHECK_EQUAL(jwk.at("use").get<std::string>(), "sig");
	BOOST_CHECK_EQUAL(jwk.at("alg").get<std::string>(), "RS256");
	BOOST_CHECK_EQUAL(jwk.at("kid").get<std::string>(), idp.Kid());
	BOOST_CHECK(!jwk.at("n").get<std::string>().empty());
	BOOST_CHECK(!jwk.at("e").get<std::string>().empty());
}

//-----------------------------------------------------------------------------
// TOKEN ISSUANCE — VERIFIES AGAINST THE JWKS KEY
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MockIdp_IssuedTokenVerifiesAgainstJwksKey)
{
	const Test::MockOidcProvider idp;

	const auto token = idp.IssueIdToken(
		"alice",
		{ "Household", "Administrators" },
		{ "openid", "profile" },
		nlohmann::json{ { "email", "alice@example.org" } });

	const auto decoded = jwt::decode<JwtTraits>(token);

	// The kid header must point at the advertised JWKS key.
	BOOST_REQUIRE(decoded.has_key_id());
	BOOST_CHECK_EQUAL(decoded.get_key_id(), idp.Kid());

	// RS256 verification with the public key REBUILT from the JWKS n/e.
	const auto public_pem = PemFromJwks(idp.Jwks());

	BOOST_CHECK_NO_THROW(
		jwt::verify<JwtTraits>()
			.allow_algorithm(jwt::algorithm::rs256{ public_pem, "", "", "" })
			.with_issuer("https://mock-idp.test")
			.with_audience("aqualink-automate")
			.verify(decoded));

	// Claim round-trip: sub, groups (default claim name) and scope.
	const auto payload = nlohmann::json::parse(decoded.get_payload());

	BOOST_CHECK_EQUAL(payload.at("sub").get<std::string>(), "alice");
	BOOST_CHECK_EQUAL(payload.at("scope").get<std::string>(), "openid profile");
	BOOST_CHECK_EQUAL(payload.at("email").get<std::string>(), "alice@example.org");

	const auto groups = payload.at("groups").get<std::vector<std::string>>();

	BOOST_REQUIRE_EQUAL(groups.size(), 2u);
	BOOST_CHECK_EQUAL(groups[0], "Household");
	BOOST_CHECK_EQUAL(groups[1], "Administrators");
}

BOOST_AUTO_TEST_CASE(Test_MockIdp_ConfigurableIssuerAudienceAndGroupsClaim)
{
	const Test::MockOidcProvider idp(Test::MockOidcProvider::Config{
		.Issuer = "https://idp.example.org",
		.Audience = "some-client-id",
		.GroupsClaim = "roles" });

	const auto decoded = jwt::decode<JwtTraits>(idp.IssueIdToken("bob", { "Guests" }));

	const auto public_pem = PemFromJwks(idp.Jwks());

	BOOST_CHECK_NO_THROW(
		jwt::verify<JwtTraits>()
			.allow_algorithm(jwt::algorithm::rs256{ public_pem, "", "", "" })
			.with_issuer("https://idp.example.org")
			.with_audience("some-client-id")
			.verify(decoded));

	// The groups claim travels under the CONFIGURED name, not the default.
	const auto payload = nlohmann::json::parse(decoded.get_payload());

	BOOST_CHECK(!payload.contains("groups"));
	BOOST_REQUIRE(payload.contains("roles"));
	BOOST_CHECK_EQUAL(payload.at("roles").get<std::vector<std::string>>().at(0), "Guests");
}

//-----------------------------------------------------------------------------
// NEGATIVE KNOBS — WRONG KEY, EXPIRED
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MockIdp_WrongKeyTokenFailsJwksVerification)
{
	const Test::MockOidcProvider idp;

	const auto token = idp.IssueIdToken(
		"mallory", {}, {}, nlohmann::json::object(), 5min,
		Test::MockOidcProvider::TokenOptions{ .UseWrongKey = true });

	const auto decoded = jwt::decode<JwtTraits>(token);

	// Same kid as the advertised key (a real verifier would select it) …
	BOOST_CHECK_EQUAL(decoded.get_key_id(), idp.Kid());

	// … but the signature must NOT verify against the JWKS key.
	const auto public_pem = PemFromJwks(idp.Jwks());

	BOOST_CHECK_THROW(
		jwt::verify<JwtTraits>()
			.allow_algorithm(jwt::algorithm::rs256{ public_pem, "", "", "" })
			.with_issuer("https://mock-idp.test")
			.with_audience("aqualink-automate")
			.verify(decoded),
		std::system_error);
}

BOOST_AUTO_TEST_CASE(Test_MockIdp_ExpiredTokenFailsExpiryVerification)
{
	const Test::MockOidcProvider idp;

	const auto token = idp.IssueIdToken(
		"alice", {}, {}, nlohmann::json::object(), 5min,
		Test::MockOidcProvider::TokenOptions{ .Expired = true });

	const auto decoded = jwt::decode<JwtTraits>(token);

	// Signature/issuer/audience are all fine — ONLY the expiry is in the past.
	const auto public_pem = PemFromJwks(idp.Jwks());

	BOOST_CHECK(decoded.get_expires_at() < std::chrono::system_clock::now());

	BOOST_CHECK_THROW(
		jwt::verify<JwtTraits>()
			.allow_algorithm(jwt::algorithm::rs256{ public_pem, "", "", "" })
			.with_issuer("https://mock-idp.test")
			.with_audience("aqualink-automate")
			.verify(decoded),
		std::system_error);
}

//-----------------------------------------------------------------------------
// MOCK TRUSTED PROXY — FORWARD-AUTH HEADER STAMPING
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_MockTrustedProxy_StampsDefaultForwardAuthHeaders)
{
	HTTP::Request req{ boost::beast::http::verb::get, "/api/equipment", 11 };

	Test::ApplyForwardAuth(req, "carol", { "Household", "Guests" });

	BOOST_REQUIRE(req.find("Remote-User") != req.end());
	BOOST_CHECK_EQUAL(req.at("Remote-User"), "carol");

	BOOST_REQUIRE(req.find("Remote-Groups") != req.end());
	BOOST_CHECK_EQUAL(req.at("Remote-Groups"), "Household,Guests");
}

BOOST_AUTO_TEST_CASE(Test_MockTrustedProxy_HonoursCustomHeaderNamesAndOmitsEmptyGroups)
{
	HTTP::Request req{ boost::beast::http::verb::get, "/api/equipment", 11 };

	Test::ApplyForwardAuth(req, "dave", {}, "X-Auth-User", "X-Auth-Groups");

	BOOST_REQUIRE(req.find("X-Auth-User") != req.end());
	BOOST_CHECK_EQUAL(req.at("X-Auth-User"), "dave");

	// No groups supplied -> no groups header stamped (matches proxy behaviour).
	BOOST_CHECK(req.find("X-Auth-Groups") == req.end());
	BOOST_CHECK(req.find("Remote-User") == req.end());
}

BOOST_AUTO_TEST_SUITE_END()
