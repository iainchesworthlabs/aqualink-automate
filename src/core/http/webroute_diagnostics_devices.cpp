#include <format>
#include <source_location>

#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_devices.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "interfaces/idescribable.h"
#include "interfaces/iemulateddevice.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	nlohmann::json CollectDeviceDiagnostics(const Kernel::EquipmentHub& equipment_hub, bool want_emulated)
	{
		nlohmann::json result = nlohmann::json::array();

		equipment_hub.ForEachDevice([&result, want_emulated](const Interfaces::IDevice& device)
			{
				// Real bus-discovered devices share the same concrete classes
				// as their emulated counterparts, so filter on emulation
				// *state* rather than on the describable type alone. A device
				// that is not an IEmulatedDevice is treated as non-emulated.
				auto emulated = dynamic_cast<const Interfaces::IEmulatedDevice*>(&device);
				if (const bool is_emulated = (nullptr != emulated) && emulated->IsEmulated(); is_emulated != want_emulated)
				{
					return;
				}

				auto describable = dynamic_cast<const Interfaces::IDescribable*>(&device);
				if (nullptr == describable)
				{
					return;
				}

				result.push_back(describable->DescribeDiagnostics());
			});

		return result;
	}

	WebRoute_Diagnostics_Devices::WebRoute_Diagnostics_Devices(Kernel::HubLocator& hub_locator) :
		// HubLocator::Find throws Hub_NotFound if the hub is not registered,
		// so a successfully-constructed route always has a valid hub pointer.
		m_EquipmentHub(hub_locator.Find<Kernel::EquipmentHub>())
	{
	}

	nlohmann::json WebRoute_Diagnostics_Devices::CollectEmulatedDiagnostics() const
	{
		return CollectDeviceDiagnostics(*m_EquipmentHub, true);
	}

	HTTP::Response WebRoute_Diagnostics_Devices::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Diagnostics_Devices::OnRequest", std::source_location::current());

		if (req.method() != Verbs::get)
		{
			LogDebug(Channel::Web, "Rejected non-GET request to the emulated-devices diagnostics endpoint");

			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET.", {{"allowed", "GET"}});
		}

		const auto result = CollectEmulatedDiagnostics();

		LogTrace(Channel::Web, std::format("Serving emulated-device diagnostics for {} device(s)", result.size()));

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
