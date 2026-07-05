#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "preferences/user_preferences_store.h"

namespace AqualinkAutomate::Preferences
{

	namespace
	{
		constexpr std::uint32_t SCHEMA_VERSION{ 1 };

		// The fixed per-user vocabulary (D7).  A system/admin field cannot slip
		// in because anything outside this set is ignored on apply.
		constexpr std::array<std::string_view, 4> PER_USER_KEYS{
			"temperature_units",
			"theme",
			"accent",
			"chemistry_bands"
		};

		bool IsKnownKey(std::string_view key)
		{
			return std::ranges::find(PER_USER_KEYS, key) != PER_USER_KEYS.end();
		}

		// Retain only recognised keys from `source` into `target` (used for both
		// defaults and stored overrides so neither can carry a stray field).
		nlohmann::json FilterKnown(const nlohmann::json& source)
		{
			nlohmann::json filtered = nlohmann::json::object();

			if (source.is_object())
			{
				for (const auto& [key, value] : source.items())
				{
					if (IsKnownKey(key))
					{
						filtered[key] = value;
					}
				}
			}

			return filtered;
		}

		// Validate one field; returns an error string when invalid.
		std::string ValidateField(std::string_view key, const nlohmann::json& value)
		{
			if ("temperature_units" == key)
			{
				if (!value.is_string() || ((value != "Celsius") && (value != "Fahrenheit")))
				{
					return "temperature_units must be 'Celsius' or 'Fahrenheit'";
				}
			}
			else if ("theme" == key)
			{
				if (!value.is_string() || ((value != "light") && (value != "dark") && (value != "system")))
				{
					return "theme must be 'light', 'dark' or 'system'";
				}
			}
			else if ("accent" == key)
			{
				if (!value.is_string() || value.get<std::string>().empty() || (value.get<std::string>().size() > 32))
				{
					return "accent must be a non-empty short string";
				}
			}
			else if (("chemistry_bands" == key) && !value.is_object())
			{
				return "chemistry_bands must be an object";
			}

			return {};
		}
	}
	// anonymous namespace

	UserPreferencesStore UserPreferencesStore::Load(const std::filesystem::path& file)
	{
		UserPreferencesStore store;
		store.m_File = file;

		if (!std::filesystem::exists(file))
		{
			return store;
		}

		std::ifstream in(file, std::ios::binary);
		nlohmann::json document;

		try
		{
			document = nlohmann::json::parse(in);
		}
		catch (const nlohmann::json::parse_error& ex)
		{
			throw std::runtime_error(std::format("User preferences file {} is unreadable ({}); refusing to continue and drop everyone's settings", file.string(), ex.what()));
		}

		if (document.value("schema_version", std::uint32_t{ 0 }) != SCHEMA_VERSION)
		{
			throw std::runtime_error(std::format("User preferences file {} has an unsupported schema version", file.string()));
		}

		if (const auto users = document.find("users"); (users != document.end()) && users->is_object())
		{
			for (const auto& [user_id, overrides] : users->items())
			{
				store.m_ByUser[user_id] = FilterKnown(overrides);
			}
		}

		return store;
	}

	void UserPreferencesStore::SetDefaults(nlohmann::json defaults)
	{
		m_Defaults = FilterKnown(defaults);
	}

	nlohmann::json UserPreferencesStore::Effective(std::string_view user_id) const
	{
		nlohmann::json effective = m_Defaults;

		if (const auto it = m_ByUser.find(std::string{ user_id }); it != m_ByUser.end())
		{
			for (const auto& [key, value] : it->items())
			{
				effective[key] = value;
			}
		}

		return effective;
	}

	nlohmann::json UserPreferencesStore::Overrides(std::string_view user_id) const
	{
		if (const auto it = m_ByUser.find(std::string{ user_id }); it != m_ByUser.end())
		{
			return *it;
		}

		return nlohmann::json::object();
	}

	bool UserPreferencesStore::Apply(std::string_view user_id, const nlohmann::json& partial, std::string& error)
	{
		if (user_id.empty())
		{
			error = "a user id is required to store per-user preferences";
			return false;
		}

		if (!partial.is_object())
		{
			error = "preferences must be a JSON object";
			return false;
		}

		// Validate every recognised field before mutating (unknown keys are
		// silently ignored, never an error, so a merged full-prefs document is
		// accepted and simply filtered).
		for (const auto& [key, value] : partial.items())
		{
			if (!IsKnownKey(key))
			{
				continue;
			}

			if (auto field_error = ValidateField(key, value); !field_error.empty())
			{
				error = std::move(field_error);
				return false;
			}
		}

		auto& overrides = m_ByUser[std::string{ user_id }];

		if (!overrides.is_object())
		{
			overrides = nlohmann::json::object();
		}

		for (const auto& [key, value] : partial.items())
		{
			if (IsKnownKey(key))
			{
				overrides[key] = value;
			}
		}

		Save();
		return true;
	}

	void UserPreferencesStore::Forget(std::string_view user_id)
	{
		if (m_ByUser.erase(std::string{ user_id }) > 0)
		{
			Save();
		}
	}

	bool UserPreferencesStore::HasOverrides(std::string_view user_id) const
	{
		return m_ByUser.contains(std::string{ user_id });
	}

	void UserPreferencesStore::Save() const
	{
		if (m_File.empty())
		{
			return;
		}

		nlohmann::json document;
		document["schema_version"] = SCHEMA_VERSION;
		document["users"] = m_ByUser;

		const auto temp_file = std::filesystem::path{ m_File }.concat(".tmp");

		{
			std::ofstream out(temp_file, std::ios::binary | std::ios::trunc);

			if (!out.is_open())
			{
				throw std::runtime_error(std::format("Could not write user preferences file {}", temp_file.string()));
			}

			out << document.dump(2);
		}

		std::error_code perm_ec;
		std::filesystem::permissions(temp_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, perm_ec);

		std::filesystem::rename(temp_file, m_File);
	}

}
// namespace AqualinkAutomate::Preferences
