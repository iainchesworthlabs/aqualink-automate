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
	inline constexpr char DIAGNOSTICS_ACTUAL_DEVICES_ROUTE_URL[] = "/api/diagnostics/actual-devices";

	// Sibling of WebRoute_Diagnostics_Devices that reports only *real*
	// (non-emulated) bus-discovered devices. The JSON schema is identical to
	// the emulated-devices route; the only difference is the emulation filter
	// (see CollectDeviceDiagnostics in webroute_diagnostics_devices.h). Real
	// devices are passive snoopers and never transmit on the RS-485 bus, so
	// this endpoint carries zero bus risk.
	class WebRoute_Diagnostics_ActualDevices : public Interfaces::IWebRoute<DIAGNOSTICS_ACTUAL_DEVICES_ROUTE_URL>
	{
	public:
		explicit WebRoute_Diagnostics_ActualDevices(Kernel::HubLocator& hub_locator);
		~WebRoute_Diagnostics_ActualDevices() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

		// Collect diagnostic JSON for every *non-emulated* device in the hub.
		nlohmann::json CollectActualDiagnostics() const;

	private:
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
	};

}
// namespace AqualinkAutomate::HTTP
