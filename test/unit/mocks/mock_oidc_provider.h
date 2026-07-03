#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Test
{

	//=========================================================================
	// MockOidcProvider — in-process mock OIDC identity provider for the auth
	// test harness (docs/auth-redesign.md §12, "Mock IdP / provider harness").
	//
	// Slice 1 models the provider as a fixture OBJECT: its methods return the
	// payloads a real IdP would serve (discovery document, JWKS, RS256-signed
	// ID tokens) so the in-app OIDC client's discovery → JWKS-verify →
	// claim-map pipeline can be tested deterministically with no network and
	// no real IdP.  A socket-serving wrapper can be layered on in Slice 4
	// when the real OIDC client needs to fetch these documents over HTTP.
	//
	// Negative-path knobs: issue with a WRONG (unadvertised) keypair so
	// signature verification against the JWKS key fails, and issue expired
	// tokens so expiry verification fails.
	//=========================================================================

	class MockOidcProvider
	{
	public:
		struct Config
		{
			std::string Issuer{ "https://mock-idp.test" };
			std::string Audience{ "aqualink-automate" };
			std::string GroupsClaim{ "groups" };
		};

		// Per-token knobs for negative tests.
		struct TokenOptions
		{
			// Sign with a second, NEVER-advertised keypair (same kid header) so
			// the token fails signature verification against the JWKS key.
			bool UseWrongKey{ false };

			// Stamp iat/exp well in the past so expiry verification fails.
			bool Expired{ false };
		};

	public:
		// No `Config config = {}` default here: a default argument evaluated
		// from within the enclosing class relies on the nested Config's default
		// member initializers before MockOidcProvider is complete. MSVC accepts
		// this non-conformingly; GCC/Clang correctly reject it (same shape as
		// PasswordHasher::Hash, see its header comment). The no-arg overload
		// below is defined out-of-line instead, where the class is complete.
		explicit MockOidcProvider(Config config);
		MockOidcProvider();

	public:
		// OIDC discovery document (RFC 8414 / OpenID Connect Discovery 1.0).
		nlohmann::json DiscoveryDocument() const;

		// JWKS advertising the (single) RSA signing key (RFC 7517).
		nlohmann::json Jwks() const;

		// An RS256-signed ID token carrying iss/aud/sub/iat/exp, the groups
		// claim (named per Config::GroupsClaim), a space-delimited "scope"
		// claim (when scopes are supplied) and any extra claims.
		//
		// Split into two overloads for the same reason as the constructor
		// above: `TokenOptions options = {}` cannot be a default argument here.
		std::string IssueIdToken(
			const std::string& subject,
			const std::vector<std::string>& groups,
			const std::vector<std::string>& scopes,
			const nlohmann::json& extra_claims,
			std::chrono::seconds ttl,
			TokenOptions options) const;

		std::string IssueIdToken(
			const std::string& subject,
			const std::vector<std::string>& groups = {},
			const std::vector<std::string>& scopes = {},
			const nlohmann::json& extra_claims = nlohmann::json::object(),
			std::chrono::seconds ttl = std::chrono::minutes{ 5 }) const;

	public:
		const Config& GetConfig() const
		{
			return m_Config;
		}

		const std::string& Kid() const
		{
			return m_Kid;
		}

		// PEM of the ADVERTISED signing key (the one in Jwks()).
		const std::string& PublicKeyPem() const
		{
			return m_SigningKey.PublicPem;
		}

	private:
		struct RsaKeyPair
		{
			std::string PublicPem{};
			std::string PrivatePem{};
			std::string ModulusB64Url{};	// JWK "n" (RFC 7518 §6.3.1.1)
			std::string ExponentB64Url{};	// JWK "e" (RFC 7518 §6.3.1.2)
		};

		static RsaKeyPair GenerateRsaKeyPair();

	private:
		Config m_Config;
		std::string m_Kid;
		RsaKeyPair m_SigningKey;	// advertised via Jwks()
		RsaKeyPair m_WrongKey;		// never advertised (negative tests)
	};

}
// namespace AqualinkAutomate::Test
