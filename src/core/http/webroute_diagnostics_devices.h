#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	// Shared collector for the device-diagnostics routes. Iterates every
	// device in the hub, keeps only those whose emulation state matches
	// `want_emulated`, and appends each survivor's DescribeDiagnostics() JSON.
	//
	// Real bus-discovered devices share the same concrete classes (and thus
	// the same IDescribable type) as their emulated counterparts, so the
	// filter is on emulation *state* (IEmulatedDevice::IsEmulated()) rather
	// than on the describable type alone. A device that is not an
	// IEmulatedDevice is treated as non-emulated.
	//
	// Side-effect free so the emulation filter can be unit tested without
	// driving a full HTTP request/response cycle.
	nlohmann::json CollectDeviceDiagnostics(const Kernel::EquipmentHub& equipment_hub, bool want_emulated);

	inline constexpr char DIAGNOSTICS_DEVICES_ROUTE_URL[] = "/api/diagnostics/emulated-devices";

	class WebRoute_Diagnostics_Devices : public Interfaces::IWebRoute<DIAGNOSTICS_DEVICES_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Devices(Kernel::HubLocator& hub_locator);
		~WebRoute_Diagnostics_Devices() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

		// Collect diagnostic JSON for every *emulated* device in the hub.
		nlohmann::json CollectEmulatedDiagnostics() const;

	private:
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
	};

}
// namespace AqualinkAutomate::HTTP
