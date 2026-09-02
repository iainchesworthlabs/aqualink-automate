#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"

namespace AqualinkAutomate::Preferences
{
	class PreferencesService;
}
// namespace AqualinkAutomate::Preferences

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENTAUXSLOT_ROUTE_URL[] = "/api/equipment/aux-slots/{aux_id}";

	// PUT-only: set (or clear) one aux slot's presence override -- "auto"/"present"/"absent".
	// SYSTEM_ADMIN-gated (this can create or delete a device, stricter than a display-name edit).
	// Persists via the same PreferencesService::ApplyJson validate/commit/atomic-file-write
	// pipeline every other preference uses (label_overrides included), then explicitly reconciles
	// the live device graph itself (PreferencesService is core and must not do this -- see
	// jandy_auxillary_presence_override.h) before reading state back, so the response and every
	// other channel (REST/WS/MQTT/HA) already reflect the change.
	//
	// Lives under src/jandy (not src/core/http, unlike most routes) for the same layering reason
	// as WebRoute_Equipment_AuxSlots.
	class WebRoute_Equipment_AuxSlot : public Interfaces::IWebRoute<EQUIPMENTAUXSLOT_ROUTE_URL>
	{
	public:
		WebRoute_Equipment_AuxSlot(Kernel::HubLocator& hub_locator, std::shared_ptr<Preferences::PreferencesService> preferences_service);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		HTTP::Response HandlePut(const HTTP::Request& req);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Kernel::PreferencesHub> m_PreferencesHub{ nullptr };
		std::shared_ptr<Preferences::PreferencesService> m_PreferencesService{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
