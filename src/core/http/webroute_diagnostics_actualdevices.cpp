#include <format>
#include <source_location>

#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_actualdevices.h"
#include "http/webroute_diagnostics_devices.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	WebRoute_Diagnostics_ActualDevices::WebRoute_Diagnostics_ActualDevices(Kernel::HubLocator& hub_locator)
	{
		// HubLocator::Find throws Hub_NotFound if the hub is not registered,
		// so a successfully-constructed route always has a valid hub pointer.
		m_EquipmentHub = hub_locator.Find<Kernel::EquipmentHub>();
	}

	nlohmann::json WebRoute_Diagnostics_ActualDevices::CollectActualDiagnostics() const
	{
		return CollectDeviceDiagnostics(*m_EquipmentHub, false);
	}

	HTTP::Response WebRoute_Diagnostics_ActualDevices::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_ActualDevices::OnRequest", std::source_location::current());

		if (req.method() != Verbs::get)
		{
			LogDebug(Channel::Web, "Rejected non-GET request to the actual-devices diagnostics endpoint");

			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET.", {{"allowed", "GET"}});
		}

		const auto result = CollectActualDiagnostics();

		LogTrace(Channel::Web, std::format("Serving actual-device diagnostics for {} device(s)", result.size()));

		HTTP::Response resp{ HTTP::Status::ok, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
		resp.keep_alive(req.keep_alive());
		resp.body() = result.dump();
		resp.prepare_payload();

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
