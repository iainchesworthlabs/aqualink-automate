#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_MQTT_ROUTE_URL[] = "/api/diagnostics/mqtt";

	// Live MQTT status for the diagnostics page: connection state, broker, HA
	// discovery, queue depth, reconnect/published/dropped counters, last error.
	// Returns { "enabled": false } when MQTT is disabled (or not yet constructed).
	class WebRoute_Diagnostics_Mqtt : public Interfaces::IWebRoute<DIAGNOSTICS_MQTT_ROUTE_URL>
	{
	public:
		explicit WebRoute_Diagnostics_Mqtt(Kernel::HubLocator& hub_locator);
		~WebRoute_Diagnostics_Mqtt() override = default;

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		// MQTT is constructed AFTER the routes are registered, so resolve it
		// lazily per-request rather than caching a (possibly-null) handle.
		Kernel::HubLocator& m_HubLocator;
	};

}
// namespace AqualinkAutomate::HTTP
