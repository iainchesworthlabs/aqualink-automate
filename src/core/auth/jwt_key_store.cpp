#include <chrono>
#include <format>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "auth/jwt_key_store.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::size_t SECRET_BYTES{ 32 };
		constexpr std::size_t KID_HEX_CHARS{ 16 };
		constexpr std::size_t MAX_RETAINED_KEYS{ 2 };  // Active + one grace key.
		constexpr std::uint32_t SCHEMA_VERSION{ 1 };

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

		std::optional<std::vector<std::uint8_t>> FromHex(std::string_view hex)
		{
			if (0 != (hex.size() % 2))
			{
				return std::nullopt;
			}

			const auto nibble = [](char ch)
			{
				if (ch >= '0' && ch <= '9') { return ch - '0'; }
				if (ch >= 'a' && ch <= 'f') { return 10 + (ch - 'a'); }
				if (ch >= 'A' && ch <= 'F') { return 10 + (ch - 'A'); }
				return -1;
			};

			std::vector<std::uint8_t> bytes;
			bytes.reserve(hex.size() / 2);

			for (std::size_t i = 0; i < hex.size(); i += 2)
			{
				const auto hi = nibble(hex[i]), lo = nibble(hex[i + 1]);

				if ((hi < 0) || (lo < 0))
				{
					return std::nullopt;
				}

				bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
			}

			return bytes;
		}

		std::string DeriveKid(const std::vector<std::uint8_t>& secret)
		{
			std::uint8_t digest[SHA256_DIGEST_LENGTH];
			SHA256(secret.data(), secret.size(), digest);

			return ToHex(digest, sizeof(digest)).substr(0, KID_HEX_CHARS);
		}
	}
	// anonymous namespace

	JwtKeyStore JwtKeyStore::LoadOrCreate(const std::filesystem::path& key_file)
	{
		JwtKeyStore store;
		store.m_KeyFile = key_file;

		if (std::filesystem::exists(key_file))
		{
			std::ifstream file(key_file);
			nlohmann::json doc;

			try
			{
				doc = nlohmann::json::parse(file);
			}
			catch (const nlohmann::json::parse_error& ex)
			{
				throw std::runtime_error(std::format("JWT key file {} is unreadable ({}); refusing to silently regenerate as that would invalidate every session", key_file.string(), ex.what()));
			}

			for (const auto& key_json : doc.value("keys", nlohmann::json::array()))
			{
				SigningKey key;
				key.Kid = key_json.value("kid", "");
				key.CreatedUnix = key_json.value("created", std::int64_t{ 0 });

				const auto secret = FromHex(key_json.value("secret_hex", ""));

				if (key.Kid.empty() || !secret.has_value() || secret->empty())
				{
					throw std::runtime_error(std::format("JWT key file {} contains a malformed key entry", key_file.string()));
				}

				key.Secret = *secret;
				store.m_Keys.push_back(std::move(key));
			}

			if (store.m_Keys.empty())
			{
				throw std::runtime_error(std::format("JWT key file {} contains no keys", key_file.string()));
			}

			return store;
		}

		store.m_Keys.push_back(GenerateKey());
		store.Save();

		return store;
	}

	const SigningKey& JwtKeyStore::Active() const
	{
		return m_Keys.front();
	}

	std::optional<SigningKey> JwtKeyStore::Find(std::string_view kid) const
	{
		for (const auto& key : m_Keys)
		{
			if (key.Kid == kid)
			{
				return key;
			}
		}

		return std::nullopt;
	}

	void JwtKeyStore::Rotate()
	{
		m_Keys.insert(m_Keys.begin(), GenerateKey());

		if (m_Keys.size() > MAX_RETAINED_KEYS)
		{
			m_Keys.resize(MAX_RETAINED_KEYS);
		}

		Save();
	}

	void JwtKeyStore::Save() const
	{
		namespace fs = std::filesystem;

		nlohmann::json doc;
		doc["schema_version"] = SCHEMA_VERSION;
		doc["active"] = m_Keys.front().Kid;

		auto keys = nlohmann::json::array();

		for (const auto& key : m_Keys)
		{
			keys.push_back({ { "kid", key.Kid }, { "secret_hex", ToHex(key.Secret.data(), key.Secret.size()) }, { "created", key.CreatedUnix } });
		}

		doc["keys"] = std::move(keys);

		// Atomic write-temp-then-rename (same pattern as the preferences and
		// schedules stores), with the key material locked to owner-only before
		// it lands at its final name.
		const auto temp_file = fs::path{ m_KeyFile }.concat(".tmp");

		{
			std::ofstream file(temp_file, std::ios::trunc);

			if (!file.is_open())
			{
				throw std::runtime_error(std::format("Could not write JWT key file {}", temp_file.string()));
			}

			file << doc.dump(2);
		}

		std::error_code perm_ec;
		fs::permissions(temp_file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, perm_ec);

		fs::rename(temp_file, m_KeyFile);
	}

	SigningKey JwtKeyStore::GenerateKey()
	{
		SigningKey key;
		key.Secret.resize(SECRET_BYTES);

		if (1 != RAND_bytes(key.Secret.data(), static_cast<int>(key.Secret.size())))
		{
			throw std::runtime_error("OpenSSL RAND_bytes failed while generating a JWT signing key");
		}

		key.Kid = DeriveKid(key.Secret);
		key.CreatedUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		return key;
	}

}
// namespace AqualinkAutomate::Auth
