#include <algorithm>
#include <vector>

#include "auth/auth_store_file.h"
#include "auth/group_store.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr bool IsBuiltInName(std::string_view name)
		{
			return (BuiltInGroups::EVERYONE == name) || (BuiltInGroups::GUEST == name) || (BuiltInGroups::ADMINISTRATORS == name);
		}
	}
	// anonymous namespace

	GroupStore GroupStore::Load(const std::filesystem::path& file)
	{
		GroupStore store;
		store.m_File = file;
		*store.m_Registry = GroupRegistry::WithBuiltIns();

		if (const auto document = LoadAuthStoreFile(file); document.has_value())
		{
			for (const auto& group_json : document->value("groups", nlohmann::json::array()))
			{
				Group group;
				group.Name = group_json.value("name", "");
				group.Entitlements = EntitlementSet::Parse(group_json.value("entitlements", std::vector<std::string>{}));
				group.BuiltIn = IsBuiltInName(group.Name);

				if (!group.Name.empty())
				{
					// Built-ins were pre-seeded; the file's entry overrides their
					// ENTITLEMENTS (the admin-scoped part) but never removes them.
					store.m_Registry->Upsert(std::move(group));
				}
			}
		}
		else
		{
			store.Save();  // First run: persist the seeded built-ins.
		}

		return store;
	}

	bool GroupStore::Upsert(Group group, std::string& error)
	{
		if (group.Name.empty())
		{
			error = "Group name is required";
			return false;
		}

		group.BuiltIn = IsBuiltInName(group.Name);

		m_Registry->Upsert(std::move(group));
		Save();

		return true;
	}

	bool GroupStore::Remove(std::string_view name, std::string& error)
	{
		if (IsBuiltInName(name))
		{
			error = "Built-in groups cannot be removed";
			return false;
		}

		if (!m_Registry->Find(name).has_value())
		{
			error = "Group not found";
			return false;
		}

		// Rebuild without the removed group (the registry has no erase; the
		// collection is tiny and this stays obviously-correct).  Assign INTO
		// the shared object — the subject resolver holds the same handle and
		// must see the change immediately.
		GroupRegistry rebuilt;

		for (const auto& group : m_Registry->All())
		{
			if (group.Name != name)
			{
				rebuilt.Upsert(group);
			}
		}

		*m_Registry = std::move(rebuilt);
		Save();

		return true;
	}

	void GroupStore::Save() const
	{
		nlohmann::json document;
		auto groups = nlohmann::json::array();

		for (const auto& group : m_Registry->All())
		{
			groups.push_back({ { "name", group.Name }, { "entitlements", group.Entitlements.ToStrings() } });
		}

		document["groups"] = std::move(groups);

		SaveAuthStoreFile(m_File, std::move(document));
	}

}
// namespace AqualinkAutomate::Auth
