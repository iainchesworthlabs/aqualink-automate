#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_LOGGING_ROUTE_URL[] = "/api/diagnostics/logging";

	class WebRoute_Diagnostics_Logging : public Interfaces::IWebRoute<DIAGNOSTICS_LOGGING_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Logging() = default;
		~WebRoute_Diagnostics_Logging() override = default;

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
		HTTP::Response HandleGet(const HTTP::Request& req);
		HTTP::Response HandlePost(const HTTP::Request& req);
	};

}
// namespace AqualinkAutomate::HTTP
