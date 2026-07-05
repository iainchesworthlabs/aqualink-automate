#include "http/json/json_equipment.h"
#include "http/server/server_fields.h"
#include "http/webroute_equipment_devices.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_Devices::WebRoute_Equipment_Devices(Kernel::HubLocator& hub_locator) :
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_PreferencesHub(hub_locator.Find<Kernel::PreferencesHub>())
	{
	}
	
    HTTP::Response WebRoute_Equipment_Devices::OnRequest(const HTTP::Request& req)
    {
        auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_EquipmentDevices::OnRequest", std::source_location::current());

        HTTP::Response resp{HTTP::Status::ok, req.version()};

        resp.set(boost::beast::http::field::server, ServerFields::Server());
        resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
        resp.keep_alive(req.keep_alive());
        resp.body() = JSON::GenerateJson_Equipment_Devices(m_DataHub, m_PreferencesHub ? m_PreferencesHub->LabelOverrides : nlohmann::json::object(), m_PreferencesHub && m_PreferencesHub->ShowAuxIdInLabel).dump();
        resp.prepare_payload();

        zone->Value(resp.body().size());

        return resp;
	}

}
// namespace AqualinkAutomate::HTTP
