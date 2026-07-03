#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

#include <jwt-cpp/traits/nlohmann-json/traits.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "mocks/mock_oidc_provider.h"

namespace AqualinkAutomate::Test
{

	namespace
	{
		using JwtTraits = jwt::traits::nlohmann_json;

		using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
		using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free_all)>;
		using BignumPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

		std::string BioToString(BIO* bio)
		{
			char* data = nullptr;
			const auto len = BIO_get_mem_data(bio, &data);
			return std::string{ data, static_cast<std::size_t>(len) };
		}

		// Base64url without padding, as JWK "n"/"e" require (RFC 7518 §6.3).
		std::string BignumToBase64Url(const BIGNUM* bn)
		{
			std::string bin(static_cast<std::size_t>(BN_num_bytes(bn)), '\0');
			BN_bn2bin(bn, reinterpret_cast<unsigned char*>(bin.data()));
			return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(bin));
		}
	}
	// anonymous namespace

	MockOidcProvider::MockOidcProvider(Config config) :
		m_Config(std::move(config)),
		m_Kid("mock-idp-key-1"),
		m_SigningKey(GenerateRsaKeyPair()),
		m_WrongKey(GenerateRsaKeyPair())
	{
	}

	MockOidcProvider::MockOidcProvider() :
		MockOidcProvider(Config{})
	{
	}

	MockOidcProvider::RsaKeyPair MockOidcProvider::GenerateRsaKeyPair()
	{
		EvpPkeyPtr pkey(EVP_RSA_gen(2048), &EVP_PKEY_free);

		if (!pkey)
		{
			throw std::runtime_error("MockOidcProvider: EVP_RSA_gen(2048) failed");
		}

		RsaKeyPair pair;

		{
			BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free_all);

			if (!bio || (1 != PEM_write_bio_PUBKEY(bio.get(), pkey.get())))
			{
				throw std::runtime_error("MockOidcProvider: could not serialise public key PEM");
			}

			pair.PublicPem = BioToString(bio.get());
		}

		{
			BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free_all);

			if (!bio || (1 != PEM_write_bio_PrivateKey(bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr)))
			{
				throw std::runtime_error("MockOidcProvider: could not serialise private key PEM");
			}

			pair.PrivatePem = BioToString(bio.get());
		}

		{
			BIGNUM* n_raw = nullptr;
			BIGNUM* e_raw = nullptr;

			if ((1 != EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_N, &n_raw)) ||
				(1 != EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_E, &e_raw)))
			{
				BN_free(n_raw);
				BN_free(e_raw);
				throw std::runtime_error("MockOidcProvider: could not extract RSA modulus/exponent");
			}

			BignumPtr n(n_raw, &BN_free);
			BignumPtr e(e_raw, &BN_free);

			pair.ModulusB64Url = BignumToBase64Url(n.get());
			pair.ExponentB64Url = BignumToBase64Url(e.get());
		}

		return pair;
	}

	nlohmann::json MockOidcProvider::DiscoveryDocument() const
	{
		return
		{
			{ "issuer", m_Config.Issuer },
			{ "authorization_endpoint", m_Config.Issuer + "/authorize" },
			{ "token_endpoint", m_Config.Issuer + "/token" },
			{ "jwks_uri", m_Config.Issuer + "/jwks" },
			{ "response_types_supported", nlohmann::json::array({ "code" }) },
			{ "subject_types_supported", nlohmann::json::array({ "public" }) },
			{ "id_token_signing_alg_values_supported", nlohmann::json::array({ "RS256" }) }
		};
	}

	nlohmann::json MockOidcProvider::Jwks() const
	{
		return
		{
			{ "keys", nlohmann::json::array({
				{
					{ "kty", "RSA" },
					{ "use", "sig" },
					{ "alg", "RS256" },
					{ "kid", m_Kid },
					{ "n", m_SigningKey.ModulusB64Url },
					{ "e", m_SigningKey.ExponentB64Url }
				}
			}) }
		};
	}

	std::string MockOidcProvider::IssueIdToken(
		const std::string& subject,
		const std::vector<std::string>& groups,
		const std::vector<std::string>& scopes,
		const nlohmann::json& extra_claims,
		std::chrono::seconds ttl,
		TokenOptions options) const
	{
		const auto& key = options.UseWrongKey ? m_WrongKey : m_SigningKey;

		// An expired token needs iat/exp safely in the past — well beyond any
		// leeway a verifier under test might reasonably be configured with.
		const auto now = std::chrono::system_clock::now();
		const auto issued_at = options.Expired ? (now - std::chrono::hours{ 2 }) : now;

		auto builder = jwt::create<JwtTraits>()
			.set_type("JWT")
			.set_key_id(m_Kid)
			.set_issuer(m_Config.Issuer)
			.set_audience(m_Config.Audience)
			.set_subject(subject)
			.set_issued_at(issued_at)
			.set_expires_at(issued_at + ttl)
			.set_payload_claim(m_Config.GroupsClaim, jwt::basic_claim<JwtTraits>(nlohmann::json(groups)));

		if (!scopes.empty())
		{
			// OIDC "scope" travels as a single space-delimited string.
			std::string scope;

			for (const auto& entry : scopes)
			{
				if (!scope.empty())
				{
					scope += ' ';
				}

				scope += entry;
			}

			builder.set_payload_claim("scope", jwt::basic_claim<JwtTraits>(nlohmann::json(scope)));
		}

		for (const auto& [claim_name, claim_value] : extra_claims.items())
		{
			builder.set_payload_claim(claim_name, jwt::basic_claim<JwtTraits>(claim_value));
		}

		return builder.sign(jwt::algorithm::rs256{ key.PublicPem, key.PrivatePem, "", "" });
	}

	std::string MockOidcProvider::IssueIdToken(
		const std::string& subject,
		const std::vector<std::string>& groups,
		const std::vector<std::string>& scopes,
		const nlohmann::json& extra_claims,
		std::chrono::seconds ttl) const
	{
		return IssueIdToken(subject, groups, scopes, extra_claims, ttl, TokenOptions{});
	}

}
// namespace AqualinkAutomate::Test
