#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENTAUXSLOTS_ROUTE_URL[] = "/api/equipment/aux-slots";

	// GET-only collection view over the full addressable aux-id space (bounded by the detected
	// panel model when known), merging live device state with the operator's presence/label
	// overrides. Backs the Settings "Device Names" table's "Other aux slots" tab. Mutating a
	// single slot's presence override is a separate route (WebRoute_Equipment_AuxSlot, PUT
	// .../aux-slots/{aux_id}) since it is a SYSTEM_ADMIN-gated write, stricter than this
	// EQUIPMENT_VIEW-gated read.
	//
	// Lives under src/jandy (not src/core/http, unlike most routes) because aux ids and power
	// centres are Jandy-protocol concepts -- libaqualink-automate (core) must never depend on
	// libaqualink-jandy. Registered into HTTP::Routing from aqualink-automate.cpp exactly like
	// any other route; the routing machinery itself doesn't care where a route class is defined.
	class WebRoute_Equipment_AuxSlots : public Interfaces::IWebRoute<EQUIPMENTAUXSLOTS_ROUTE_URL>
	{
	public:
		explicit WebRoute_Equipment_AuxSlots(Kernel::HubLocator& hub_locator);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
		}

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Kernel::PreferencesHub> m_PreferencesHub{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
