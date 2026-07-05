#include <exception>
#include <format>

#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "auth/jwt_codec.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		using JwtTraits = jwt::traits::nlohmann_json;

		// jwt-cpp validates exp/nbf against a Clock concept; wrap the injected
		// NowFn so tests can drive time deterministically.
		struct InjectedClock
		{
			JwtCodec::NowFn Now;

			std::chrono::system_clock::time_point now() const
			{
				return Now();
			}
		};

		std::string SecretAsString(const SigningKey& key)
		{
			return std::string{ reinterpret_cast<const char*>(key.Secret.data()), key.Secret.size() };
		}
	}
	// anonymous namespace

	JwtCodec::JwtCodec(std::shared_ptr<JwtKeyStore> key_store, Config config) :
		m_KeyStore(std::move(key_store)),
		m_Config(std::move(config))
	{
	}

	std::string JwtCodec::Sign(const TokenClaims& claims) const
	{
		const auto& key = m_KeyStore->Active();
		const auto now = m_Config.Now();

		auto builder = jwt::create<JwtTraits>()
			.set_issuer(m_Config.Issuer)
			.set_audience(m_Config.Audience)
			.set_subject(claims.Subject)
			.set_issued_at(now)
			.set_expires_at(now + m_Config.AccessTokenTtl)
			.set_key_id(key.Kid)
			.set_payload_claim("prv", jwt::basic_claim<JwtTraits>(nlohmann::json(magic_enum::enum_name(claims.Provider))))
			.set_payload_claim("tokver", jwt::basic_claim<JwtTraits>(nlohmann::json(claims.TokenVersion)))
			.set_payload_claim("grp", jwt::basic_claim<JwtTraits>(nlohmann::json(claims.Groups)));

		// Size-overflow rule (docs/auth-redesign.md §5): entitlements travel in
		// the token as the fast path, but a large per-aux grant list must not
		// push the cookie past its ceiling — elide and mark `entof` instead;
		// resolution then falls back to the server-side stores via `grp`.
		if (const nlohmann::json ent_json(claims.Entitlements); ent_json.dump().size() <= m_Config.EntClaimBudgetBytes)
		{
			builder.set_payload_claim("ent", jwt::basic_claim<JwtTraits>(ent_json));
		}
		else
		{
			builder.set_payload_claim("entof", jwt::basic_claim<JwtTraits>(nlohmann::json(true)));
		}

		return builder.sign(jwt::algorithm::hs256{ SecretAsString(key) });
	}

	std::optional<TokenClaims> JwtCodec::Verify(const std::string& token, std::string* error) const
	{
		const auto fail = [&](std::string_view reason) -> std::optional<TokenClaims>
		{
			if (nullptr != error)
			{
				*error = std::string{ reason };
			}

			return std::nullopt;
		};

		try
		{
			const auto decoded = jwt::decode<JwtTraits>(token);

			if (!decoded.has_key_id())
			{
				return fail("token has no kid header");
			}

			const auto key = m_KeyStore->Find(decoded.get_key_id());

			if (!key.has_value())
			{
				return fail(std::format("token kid {} matches no known signing key", decoded.get_key_id()));
			}

			jwt::verify<InjectedClock, JwtTraits>(InjectedClock{ m_Config.Now })
				.allow_algorithm(jwt::algorithm::hs256{ SecretAsString(*key) })
				.with_issuer(m_Config.Issuer)
				.with_audience(m_Config.Audience)
				.leeway(static_cast<std::size_t>(m_Config.LeewaySeconds.count()))
				.verify(decoded);

			// Signature/issuer/audience/expiry verified; lift the claims out of
			// the payload directly (nlohmann is the traits type anyway).
			const auto payload = nlohmann::json::parse(decoded.get_payload());

			TokenClaims claims;
			claims.Subject = payload.value("sub", "");
			claims.TokenVersion = payload.value("tokver", std::uint32_t{ 0 });
			claims.Groups = payload.value("grp", std::vector<std::string>{});
			claims.IssuedAt = decoded.get_issued_at();
			claims.ExpiresAt = decoded.get_expires_at();

			const auto provider = magic_enum::enum_cast<SubjectProvider>(payload.value("prv", ""));

			if (!provider.has_value())
			{
				return fail("token carries an unknown provider claim");
			}

			claims.Provider = *provider;

			if (payload.contains("ent"))
			{
				claims.Entitlements = payload.value("ent", std::vector<std::string>{});
				claims.EntitlementsInToken = true;
			}
			else
			{
				claims.Entitlements.clear();
				claims.EntitlementsInToken = false;
			}

			return claims;
		}
		catch (const std::exception& ex)
		{
			return fail(ex.what());
		}
	}

}
// namespace AqualinkAutomate::Auth
