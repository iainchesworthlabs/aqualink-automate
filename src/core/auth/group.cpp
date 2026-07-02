#include <algorithm>

#include "auth/entitlement_vocabulary.h"
#include "auth/group.h"

namespace AqualinkAutomate::Auth
{

	GroupRegistry GroupRegistry::WithBuiltIns()
	{
		GroupRegistry registry;

		registry.Upsert(Group{ .Name = std::string{ BuiltInGroups::EVERYONE }, .Entitlements = {}, .BuiltIn = true });
		registry.Upsert(Group{ .Name = std::string{ BuiltInGroups::GUEST }, .Entitlements = {}, .BuiltIn = true });

		EntitlementSet admin_entitlements;
		admin_entitlements.Add(Entitlement{ std::string{ Vocabulary::SYSTEM_ADMIN } });

		registry.Upsert(Group{ .Name = std::string{ BuiltInGroups::ADMINISTRATORS }, .Entitlements = std::move(admin_entitlements), .BuiltIn = true });

		return registry;
	}

	void GroupRegistry::Upsert(Group group)
	{
		const auto it = std::ranges::find_if(m_Groups, [&](const auto& existing)
			{
				return existing.Name == group.Name;
			});

		if (it != m_Groups.end())
		{
			*it = std::move(group);
		}
		else
		{
			m_Groups.push_back(std::move(group));
		}
	}

	std::optional<Group> GroupRegistry::Find(std::string_view name) const
	{
		const auto it = std::ranges::find_if(m_Groups, [&](const auto& group)
			{
				return group.Name == name;
			});

		if (m_Groups.end() == it)
		{
			return std::nullopt;
		}

		return *it;
	}

	EntitlementSet GroupRegistry::ResolveEffectiveEntitlements(const EntitlementSet& direct, const std::vector<std::string>& memberships) const
	{
		EntitlementSet effective{ direct };

		// Everyone's entitlements apply to every subject, member or not.
		if (const auto everyone = Find(BuiltInGroups::EVERYONE); everyone.has_value())
		{
			effective.Merge(everyone->Entitlements);
		}

		for (const auto& membership : memberships)
		{
			if (BuiltInGroups::EVERYONE == membership)
			{
				continue; // Already applied above.
			}

			if (const auto group = Find(membership); group.has_value())
			{
				effective.Merge(group->Entitlements);
			}
		}

		return effective;
	}

}
// namespace AqualinkAutomate::Auth
