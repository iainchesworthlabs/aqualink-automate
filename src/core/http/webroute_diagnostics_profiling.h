#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iprofilingcontroller.h"
#include "interfaces/iwebroute.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_PROFILING_ROUTE_URL[] = "/api/diagnostics/profiling";

	class WebRoute_Diagnostics_Profiling : public Interfaces::IWebRoute<DIAGNOSTICS_PROFILING_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Profiling(Kernel::HubLocator& hub_locator);
		~WebRoute_Diagnostics_Profiling() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::DIAGNOSTICS_VIEW };
			}

			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		HTTP::Response HandleGet(const HTTP::Request& req);
		HTTP::Response HandlePost(const HTTP::Request& req);

	private:
		// Owning is fine here (unlike the recording controller, which aliases the
		// serial chain): the ProfilingController is a standalone HubLocator service.
		// nullptr only if registration was somehow skipped, in which case GET reports
		// enabled=false and POST returns 503.
		std::shared_ptr<Interfaces::IProfilingController> m_ProfilingController;
	};

}
// namespace AqualinkAutomate::HTTP
