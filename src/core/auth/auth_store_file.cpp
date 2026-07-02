#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "auth/auth_store_file.h"

namespace AqualinkAutomate::Auth
{

	std::optional<nlohmann::json> LoadAuthStoreFile(const std::filesystem::path& file)
	{
		namespace fs = std::filesystem;

		if (!fs::exists(file))
		{
			return std::nullopt;
		}

		std::ifstream stream(file);
		nlohmann::json document;

		try
		{
			document = nlohmann::json::parse(stream);
		}
		catch (const nlohmann::json::parse_error& ex)
		{
			throw std::runtime_error(std::format("Auth store {} is unreadable ({}); refusing to continue with partial identity data", file.string(), ex.what()));
		}

		const auto schema_version = document.value("schema_version", std::uint32_t{ 0 });

		if (schema_version != AUTH_STORE_SCHEMA_VERSION)
		{
			// The migration hook: when the schema evolves, migrate here (and only
			// here) before returning.  An UNKNOWN (newer) version is fatal — a
			// downgrade must not silently discard fields it does not understand.
			throw std::runtime_error(std::format("Auth store {} has schema version {} (expected {})", file.string(), schema_version, AUTH_STORE_SCHEMA_VERSION));
		}

		return document;
	}

	void SaveAuthStoreFile(const std::filesystem::path& file, nlohmann::json document)
	{
		namespace fs = std::filesystem;

		document["schema_version"] = AUTH_STORE_SCHEMA_VERSION;

		const auto temp_file = fs::path{ file }.concat(".tmp");

		{
			std::ofstream stream(temp_file, std::ios::trunc);

			if (!stream.is_open())
			{
				throw std::runtime_error(std::format("Could not write auth store {}", temp_file.string()));
			}

			stream << document.dump(2);
		}

		std::error_code perm_ec;
		fs::permissions(temp_file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, perm_ec);

		fs::rename(temp_file, file);
	}

}
// namespace AqualinkAutomate::Auth
