#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Preferences
{

	//=========================================================================
	// UserPreferencesStore — per-user preference overrides (docs/auth-redesign
	// §8, D7).
	//
	// The PreferencesService owns the SYSTEM/admin preferences (alert
	// thresholds, retention, label overrides, spa-switch mapping) in its
	// single global file.  This store holds the PER-USER slice — temperature
	// units, theme, accent, chemistry-display bands — keyed by user id, in one
	// schema-versioned atomic JSON map (<auth-state-dir>/user_preferences.json,
	// owner-only), consistent with the auth stores.
	//
	// Resolution is defaults + overrides: Effective(user_id) returns the
	// GLOBAL defaults (set via SetDefaults) with the user's stored fields
	// layered on top.  An anonymous/guest caller has no id and never reaches
	// this store — the frontend keeps their choices in localStorage.
	//
	// The set of per-user keys is fixed (PER_USER_KEYS): unknown keys in an
	// applied document are ignored, so a system/admin field can never leak in.
	//=========================================================================
	class UserPreferencesStore
	{
	public:
		static UserPreferencesStore Load(const std::filesystem::path& file);

	public:
		// The global fallback for every per-user field (from the system prefs /
		// built-in defaults).  Only the recognised PER_USER_KEYS are retained.
		void SetDefaults(nlohmann::json defaults);

		// Defaults with the user's stored overrides layered on top.
		nlohmann::json Effective(std::string_view user_id) const;

		// The user's RAW stored overrides only (no defaults), empty object when
		// none.  The web route uses this to overlay a user's choices on top of
		// the LIVE global preferences, so an un-overridden field still reflects
		// the current global default ("global defaults exist too", D7).
		nlohmann::json Overrides(std::string_view user_id) const;

		// Merge a partial document into the user's overrides (recognised keys
		// only) and persist.  Returns false + error on a malformed field.
		bool Apply(std::string_view user_id, const nlohmann::json& partial, std::string& error);

		// Drop a user's overrides (called when the account is deleted).
		void Forget(std::string_view user_id);

		bool HasOverrides(std::string_view user_id) const;

	private:
		UserPreferencesStore() = default;

		void Save() const;

	private:
		std::filesystem::path m_File{};
		nlohmann::json m_Defaults{ nlohmann::json::object() };
		nlohmann::json m_ByUser{ nlohmann::json::object() };   // user_id -> overrides object.
	};

}
// namespace AqualinkAutomate::Preferences
