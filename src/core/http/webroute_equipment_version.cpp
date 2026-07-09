#include "http/json/json_equipment.h"
#include "http/server/make_response.h"
#include "http/webroute_equipment_version.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_Version::WebRoute_Equipment_Version(Kernel::HubLocator& hub_locator) :
		Interfaces::IWebRoute<EQUIPMENTVERSION_ROUTE_URL>(),
		m_DataHub(hub_locator.Find<Kernel::DataHub>())
	{
	}

	HTTP::Response WebRoute_Equipment_Version::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_EquipmentVersion::OnRequest", std::source_location::current());

		auto resp = MakeJsonResponse(req, HTTP::Status::ok, JSON::GenerateJson_Equipment_Version(m_DataHub).dump());

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
