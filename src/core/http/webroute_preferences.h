#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Preferences { class PreferencesService; class UserPreferencesStore; }

namespace AqualinkAutomate::HTTP
{
	inline constexpr char PREFERENCES_ROUTE_URL[] = "/api/preferences";

	// GET returns the effective preferences for the caller; PUT validates +
	// applies a (partial) document.  400 on a bad value, 503 if the service is
	// unavailable.
	//
	// SUBJECT-AWARE (docs/auth-redesign.md §8, D7) when the identity system is
	// on:
	//   - SYSTEM/admin fields (alert, history, label_overrides,
	//     show_aux_id_in_label, ui, spa_switch_buttons) live in the global
	//     PreferencesService and may be written only by a system.admin subject;
	//   - PER-USER fields (temperature_units, theme, accent, chemistry_bands)
	//     live in the UserPreferencesStore keyed by the caller's id.  GET
	//     overlays the caller's overrides on the live global values (so an
	//     un-set field still shows the global default); PUT routes them to the
	//     caller's own slice.
	// With auth-mode disabled there is no identity: the route behaves exactly
	// as before (everything global; theme/accent are client-side localStorage).
	class WebRoute_Preferences : public Interfaces::IWebRoute<PREFERENCES_ROUTE_URL>
	{
	public:
		WebRoute_Preferences(std::shared_ptr<Preferences::PreferencesService> service, std::shared_ptr<Preferences::UserPreferencesStore> user_prefs = {});
		~WebRoute_Preferences() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		// PREFS_SELF: any authenticated subject may read/write their OWN slice.
		// Writes to system/admin fields are additionally gated on system.admin
		// inside the handler (a regular user PUTting a system field gets 403).
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::PREFS_SELF };
		}

	private:
		std::shared_ptr<Preferences::PreferencesService> m_Service;
		std::shared_ptr<Preferences::UserPreferencesStore> m_UserPrefs;
	};

}
// namespace AqualinkAutomate::HTTP
