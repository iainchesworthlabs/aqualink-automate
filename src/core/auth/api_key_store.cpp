#include <algorithm>
#include <stdexcept>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/rand.h>

#include "auth/api_key_store.h"
#include "auth/auth_store_file.h"
#include "auth/entitlement_vocabulary.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::string_view KEY_PREFIX{ "aak_" };
		constexpr std::string_view BOOTSTRAP_KEY_ID{ "bootstrap-legacy-token" };
		constexpr std::size_t KEY_RANDOM_BYTES{ 32 };

		std::string ToHex(const std::uint8_t* data, std::size_t length)
		{
			static constexpr char HEX_DIGITS[] = "0123456789abcdef";

			std::string hex;
			hex.reserve(length * 2);

			for (std::size_t i = 0; i < length; ++i)
			{
				hex.push_back(HEX_DIGITS[data[i] >> 4]);
				hex.push_back(HEX_DIGITS[data[i] & 0x0F]);
			}

			return hex;
		}

		nlohmann::json ToJson(const ApiKeyRecord& key)
		{
			return {
				{ "id", key.Id },
				{ "label", key.Label },
				{ "secret_sha256", key.SecretSha256Hex },
				{ "entitlements", key.Entitlements.ToStrings() },
				{ "expiry", key.ExpiryUnix },
				{ "last_used", key.LastUsedUnix },
				{ "revoked", key.Revoked }
			};
		}

		ApiKeyRecord FromJson(const nlohmann::json& json)
		{
			ApiKeyRecord key;
			key.Id = json.value("id", "");
			key.Label = json.value("label", "");
			key.SecretSha256Hex = json.value("secret_sha256", "");
			key.Entitlements = EntitlementSet::Parse(json.value("entitlements", std::vector<std::string>{}));
			key.ExpiryUnix = json.value("expiry", std::int64_t{ 0 });
			key.LastUsedUnix = json.value("last_used", std::int64_t{ 0 });
			key.Revoked = json.value("revoked", false);
			return key;
		}
	}
	// anonymous namespace

	ApiKeyStore ApiKeyStore::Load(const std::filesystem::path& file)
	{
		ApiKeyStore store;
		store.m_File = file;

		if (const auto document = LoadAuthStoreFile(file); document.has_value())
		{
			for (const auto& key_json : document->value("keys", nlohmann::json::array()))
			{
				store.m_Keys.push_back(FromJson(key_json));
			}
		}

		return store;
	}

	std::string ApiKeyStore::DigestOf(std::string_view secret)
	{
		return Sha256Hex(secret);
	}

	std::optional<ApiKeyRecord> ApiKeyStore::FindById(std::string_view id) const
	{
		const auto it = std::ranges::find_if(m_Keys, [&](const auto& key) { return key.Id == id; });
		return (m_Keys.end() == it) ? std::nullopt : std::optional{ *it };
	}

	std::string ApiKeyStore::Create(std::string label, EntitlementSet entitlements, std::int64_t expiry_unix, std::string& out_key_id)
	{
		std::uint8_t random_bytes[KEY_RANDOM_BYTES];

		if (1 != RAND_bytes(random_bytes, static_cast<int>(sizeof(random_bytes))))
		{
			throw std::runtime_error("OpenSSL RAND_bytes failed while generating an API key");
		}

		// Hex keeps the secret URL/header-safe without a base64url helper.
		const std::string secret = std::string{ KEY_PREFIX } + ToHex(random_bytes, sizeof(random_bytes));

		ApiKeyRecord key;
		key.Id = boost::uuids::to_string(boost::uuids::random_generator()());
		key.Label = std::move(label);
		key.SecretSha256Hex = DigestOf(secret);
		key.Entitlements = std::move(entitlements);
		key.ExpiryUnix = expiry_unix;

		out_key_id = key.Id;

		m_Keys.push_back(std::move(key));
		Save();

		// The only moment the secret exists outside the caller's hands.
		return secret;
	}

	void ApiKeyStore::SeedBootstrapKey(std::string_view legacy_token)
	{
		if (legacy_token.empty())
		{
			return;
		}

		const auto digest = DigestOf(legacy_token);

		const auto it = std::ranges::find_if(m_Keys, [&](const auto& key) { return key.Id == BOOTSTRAP_KEY_ID; });

		if ((it != m_Keys.end()) && (it->SecretSha256Hex == digest))
		{
			return;  // Already seeded with this token.
		}

		ApiKeyRecord bootstrap;
		bootstrap.Id = std::string{ BOOTSTRAP_KEY_ID };
		bootstrap.Label = "Legacy --api-auth-token (bootstrap)";
		bootstrap.SecretSha256Hex = digest;
		bootstrap.Entitlements.Add(Entitlement{ std::string{ Vocabulary::SYSTEM_ADMIN } });

		if (it != m_Keys.end())
		{
			*it = std::move(bootstrap);  // Token changed: reseed.
		}
		else
		{
			m_Keys.push_back(std::move(bootstrap));
		}

		Save();
	}

	std::optional<ApiKeyRecord> ApiKeyStore::Authenticate(std::string_view presented_secret, std::int64_t now_unix)
	{
		if (presented_secret.empty())
		{
			return std::nullopt;
		}

		const auto digest = DigestOf(presented_secret);

		const auto it = std::ranges::find_if(m_Keys, [&](const auto& key) { return key.SecretSha256Hex == digest; });

		if (m_Keys.end() == it)
		{
			return std::nullopt;
		}

		if (it->Revoked || ((0 != it->ExpiryUnix) && (now_unix >= it->ExpiryUnix)))
		{
			return std::nullopt;
		}

		it->LastUsedUnix = now_unix;
		Save();

		return *it;
	}

	bool ApiKeyStore::Revoke(std::string_view id, std::string& error)
	{
		const auto it = std::ranges::find_if(m_Keys, [&](const auto& key) { return key.Id == id; });

		if (m_Keys.end() == it)
		{
			error = "API key not found";
			return false;
		}

		it->Revoked = true;
		Save();

		return true;
	}

	void ApiKeyStore::Save() const
	{
		nlohmann::json document;
		auto keys = nlohmann::json::array();

		for (const auto& key : m_Keys)
		{
			keys.push_back(ToJson(key));
		}

		document["keys"] = std::move(keys);

		SaveAuthStoreFile(m_File, std::move(document));
	}

}
// namespace AqualinkAutomate::Auth
