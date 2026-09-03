#include "http/webroute_equipment_auxslots.h"

#include "http/json/json_equipment_auxslots.h"
#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_AuxSlots::WebRoute_Equipment_AuxSlots(Kernel::HubLocator& hub_locator) :
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_PreferencesHub(hub_locator.TryFind<Kernel::PreferencesHub>())
	{
	}

	HTTP::Response WebRoute_Equipment_AuxSlots::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_AuxSlots::OnRequest", std::source_location::current());

		if ((HTTP::Verbs::get != req.method()) && (HTTP::Verbs::head != req.method()))
		{
			return HTTP::Responses::Response_405(req);
		}

		const nlohmann::json label_overrides = m_PreferencesHub ? m_PreferencesHub->LabelOverrides : nlohmann::json::object();
		const bool show_aux_id = m_PreferencesHub && m_PreferencesHub->ShowAuxIdInLabel;
		const nlohmann::json presence_overrides = m_PreferencesHub ? m_PreferencesHub->AuxPresenceOverrides : nlohmann::json::object();

		nlohmann::json body;
		body["slots"] = JSON::GenerateJson_Equipment_AuxSlots(m_DataHub, label_overrides, show_aux_id, presence_overrides);

		return MakeJsonResponse(req, HTTP::Status::ok, body.dump());
	}

}
// namespace AqualinkAutomate::HTTP
