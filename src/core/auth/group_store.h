#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "auth/group.h"

namespace AqualinkAutomate::Auth
{

	//=========================================================================
	// GroupStore — persistence for the GroupRegistry (docs/auth-redesign.md
	// §9): <auth-state-dir>/groups.json, schema-versioned, atomic, owner-only.
	//
	// Built-in groups (Everyone/Guest/Administrators) always exist: they are
	// seeded on first run, re-asserted on load (so a hand-edited file cannot
	// delete them), may have their ENTITLEMENTS edited (that is how an admin
	// scopes Guest access), but can never be removed.
	//=========================================================================
	class GroupStore
	{
	public:
		static GroupStore Load(const std::filesystem::path& file);

		const GroupRegistry& Registry() const noexcept { return *m_Registry; }

		// The LIVE registry as a shareable handle: the subject resolver holds
		// this so admin edits (e.g. guest scoping) apply to the very next
		// request without any re-wiring.
		std::shared_ptr<GroupRegistry> SharedRegistry() const noexcept { return m_Registry; }

		// Create or update a group.  Built-ins keep their BuiltIn flag no
		// matter what the caller passes.  Persists on success.
		bool Upsert(Group group, std::string& error);

		// Remove a (non-built-in) group.  Membership references in user
		// records degrade safely (unknown groups resolve to no entitlements).
		bool Remove(std::string_view name, std::string& error);

	private:
		GroupStore() = default;

		void Save() const;

	private:
		std::filesystem::path m_File{};
		std::shared_ptr<GroupRegistry> m_Registry{ std::make_shared<GroupRegistry>() };
	};

}
// namespace AqualinkAutomate::Auth
