#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char DIAGNOSTICS_OPTIONS_ROUTE_URL[] = "/api/diagnostics/options";

	class WebRoute_Diagnostics_Options : public Interfaces::IWebRoute<DIAGNOSTICS_OPTIONS_ROUTE_URL>
	{
	public:
		WebRoute_Diagnostics_Options() = default;
		~WebRoute_Diagnostics_Options() override = default;

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
	};

}
// namespace AqualinkAutomate::HTTP
