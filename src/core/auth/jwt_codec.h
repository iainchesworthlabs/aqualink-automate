#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auth/jwt_key_store.h"
#include "auth/subject.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// JwtCodec — mint and verify the app's own session access tokens
	// (docs/auth-redesign.md §5).
	//
	// Claims: iss/aud (fixed per deployment), sub, iat/exp, prv (provider),
	// tokver (revocation/propagation version), grp (group names) and ent (the
	// effective entitlement set, sorted, so tokens are self-describing and
	// directly assertable in tests).
	//
	// SIZE-OVERFLOW RULE: when the encoded entitlement claim would exceed
	// EntClaimBudgetBytes (large per-aux grant lists vs the ~4KB cookie
	// ceiling), the ent claim is OMITTED and `entof` (entitlement overflow) is
	// set true; the subject-resolution middleware then re-resolves
	// entitlements from the stores via the groups in `grp`.
	//
	// Verification tolerates LeewaySeconds of clock skew (RTC-less Pi boots)
	// and accepts any key still present in the JwtKeyStore (active + grace),
	// selected by the token's `kid` header.
	//=========================================================================

	struct TokenClaims
	{
		std::string Subject{};
		SubjectProvider Provider{ SubjectProvider::Anonymous };
		std::uint32_t TokenVersion{ 0 };
		std::vector<std::string> Groups{};
		std::vector<std::string> Entitlements{};

		// Verify(): true when the ent claim was present in the token; false
		// when the mint-side overflow rule elided it (re-resolve via Groups).
		bool EntitlementsInToken{ true };

		std::chrono::system_clock::time_point IssuedAt{};
		std::chrono::system_clock::time_point ExpiresAt{};
	};

	class JwtCodec
	{
	public:
		using NowFn = std::function<std::chrono::system_clock::time_point()>;

		struct Config
		{
			std::string Issuer{ "aqualink-automate" };
			std::string Audience{ "aqualink-automate" };
			std::chrono::seconds AccessTokenTtl{ std::chrono::minutes{ 15 } };
			std::chrono::seconds LeewaySeconds{ 60 };

			// Serialized-size budget for the ent claim before the overflow rule
			// elides it from the token (see class comment).
			std::size_t EntClaimBudgetBytes{ 2048 };

			// Injectable clock (tests drive expiry/skew deterministically).
			NowFn Now{ []() { return std::chrono::system_clock::now(); } };
		};

	public:
		JwtCodec(std::shared_ptr<JwtKeyStore> key_store, Config config);

	public:
		// Mint a signed access token for the claims (IssuedAt/ExpiresAt are
		// stamped from Config; any values in `claims` are ignored).
		std::string Sign(const TokenClaims& claims) const;

		// Verify signature (via the token's kid), issuer/audience, and expiry
		// (with leeway).  Returns std::nullopt on any failure; when `error` is
		// provided it receives a diagnostic (never echoed to clients).
		std::optional<TokenClaims> Verify(const std::string& token, std::string* error = nullptr) const;

	private:
		std::shared_ptr<JwtKeyStore> m_KeyStore;
		Config m_Config;
	};

}
// namespace AqualinkAutomate::Auth
