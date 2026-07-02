#include <algorithm>
#include <cctype>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "auth/auth_store_file.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/user_store.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		bool UsernameEquals(std::string_view lhs, std::string_view rhs)
		{
			return std::ranges::equal(lhs, rhs, [](unsigned char a, unsigned char b)
				{
					return std::tolower(a) == std::tolower(b);
				});
		}

		bool ResolvesSystemAdmin(const UserRecord& user, const GroupRegistry& registry)
		{
			return registry.ResolveEffectiveEntitlements(user.DirectEntitlements, user.Groups).Permits(Vocabulary::SYSTEM_ADMIN);
		}

		nlohmann::json ToJson(const UserRecord& user)
		{
			return {
				{ "id", user.Id },
				{ "username", user.Username },
				{ "password_hash", user.PasswordHash },
				{ "groups", user.Groups },
				{ "direct_entitlements", user.DirectEntitlements.ToStrings() },
				{ "tokver", user.TokenVersion },
				{ "disabled", user.Disabled }
			};
		}

		UserRecord FromJson(const nlohmann::json& json)
		{
			UserRecord user;
			user.Id = json.value("id", "");
			user.Username = json.value("username", "");
			user.PasswordHash = json.value("password_hash", "");
			user.Groups = json.value("groups", std::vector<std::string>{});
			user.DirectEntitlements = EntitlementSet::Parse(json.value("direct_entitlements", std::vector<std::string>{}));
			user.TokenVersion = json.value("tokver", std::uint32_t{ 1 });
			user.Disabled = json.value("disabled", false);
			return user;
		}
	}
	// anonymous namespace

	UserStore UserStore::Load(const std::filesystem::path& file)
	{
		UserStore store;
		store.m_File = file;

		if (const auto document = LoadAuthStoreFile(file); document.has_value())
		{
			for (const auto& user_json : document->value("users", nlohmann::json::array()))
			{
				store.m_Users.push_back(FromJson(user_json));
			}
		}

		return store;
	}

	std::optional<UserRecord> UserStore::FindById(std::string_view id) const
	{
		const auto it = std::ranges::find_if(m_Users, [&](const auto& user) { return user.Id == id; });
		return (m_Users.end() == it) ? std::nullopt : std::optional{ *it };
	}

	std::optional<UserRecord> UserStore::FindByUsername(std::string_view username) const
	{
		const auto it = std::ranges::find_if(m_Users, [&](const auto& user) { return UsernameEquals(user.Username, username); });
		return (m_Users.end() == it) ? std::nullopt : std::optional{ *it };
	}

	bool UserStore::Create(UserRecord user, std::string& error)
	{
		if (user.Username.empty() || user.PasswordHash.empty())
		{
			error = "Username and password are required";
			return false;
		}

		if (FindByUsername(user.Username).has_value())
		{
			error = "Username already exists";
			return false;
		}

		if (user.Id.empty())
		{
			user.Id = boost::uuids::to_string(boost::uuids::random_generator()());
		}

		m_Users.push_back(std::move(user));
		Save();

		return true;
	}

	bool UserStore::Update(const UserRecord& user, const GroupRegistry& registry, std::string& error)
	{
		const auto it = std::ranges::find_if(m_Users, [&](const auto& existing) { return existing.Id == user.Id; });

		if (m_Users.end() == it)
		{
			error = "User not found";
			return false;
		}

		const auto duplicate = FindByUsername(user.Username);

		if (duplicate.has_value() && (duplicate->Id != user.Id))
		{
			error = "Username already exists";
			return false;
		}

		if (WouldLoseLastAdmin(&(*it), &user, registry))
		{
			error = "Refused: this change would remove the last enabled administrator";
			return false;
		}

		*it = user;
		Save();

		return true;
	}

	bool UserStore::Remove(std::string_view id, const GroupRegistry& registry, std::string& error)
	{
		const auto it = std::ranges::find_if(m_Users, [&](const auto& user) { return user.Id == id; });

		if (m_Users.end() == it)
		{
			error = "User not found";
			return false;
		}

		if (WouldLoseLastAdmin(&(*it), nullptr, registry))
		{
			error = "Refused: this change would remove the last enabled administrator";
			return false;
		}

		m_Users.erase(it);
		Save();

		return true;
	}

	std::uint32_t UserStore::BumpTokenVersion(std::string_view id)
	{
		const auto it = std::ranges::find_if(m_Users, [&](const auto& user) { return user.Id == id; });

		if (m_Users.end() == it)
		{
			return 0;
		}

		++(it->TokenVersion);
		Save();

		return it->TokenVersion;
	}

	bool UserStore::HasEnabledAdmin(const GroupRegistry& registry) const
	{
		return std::ranges::any_of(m_Users, [&](const auto& user)
			{
				return !user.Disabled && ResolvesSystemAdmin(user, registry);
			});
	}

	bool UserStore::WouldLoseLastAdmin(const UserRecord* changed_or_removed, const UserRecord* replacement, const GroupRegistry& registry) const
	{
		// Count enabled admins as they WOULD be after the mutation.
		std::size_t admins_after = 0;

		for (const auto& user : m_Users)
		{
			const UserRecord* effective = &user;

			if ((nullptr != changed_or_removed) && (user.Id == changed_or_removed->Id))
			{
				effective = replacement;  // nullptr == removal.
			}

			if ((nullptr != effective) && !effective->Disabled && ResolvesSystemAdmin(*effective, registry))
			{
				++admins_after;
			}
		}

		// Only refuse when the system HAD an admin and the change loses it —
		// a store with no admins yet (bootstrap) may mutate freely.
		return HasEnabledAdmin(registry) && (0 == admins_after);
	}

	void UserStore::Save() const
	{
		nlohmann::json document;
		auto users = nlohmann::json::array();

		for (const auto& user : m_Users)
		{
			users.push_back(ToJson(user));
		}

		document["users"] = std::move(users);

		SaveAuthStoreFile(m_File, std::move(document));
	}

}
// namespace AqualinkAutomate::Auth
