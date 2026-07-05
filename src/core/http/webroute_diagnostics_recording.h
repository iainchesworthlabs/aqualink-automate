#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/irecordingcontroller.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_RECORDING_ROUTE_URL[] = "/api/diagnostics/recording";

	class WebRoute_Diagnostics_Recording : public Interfaces::IWebRoute<DIAGNOSTICS_RECORDING_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Recording(Kernel::HubLocator& hub_locator);
		~WebRoute_Diagnostics_Recording() override = default;

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
		HTTP::Response HandleGet(const HTTP::Request& req) const;
		HTTP::Response HandlePost(const HTTP::Request& req) const;

	private:
		// Non-owning: the controller is owned by the serial chain (SerialPort) for
		// the lifetime of the application. nullptr when no controller is registered
		// (e.g. dev-mode/replay), in which case the route reports recording=false
		// and rejects toggle attempts with 503.
		std::shared_ptr<Interfaces::IRecordingController> m_RecordingController;
	};

}
// namespace AqualinkAutomate::HTTP
