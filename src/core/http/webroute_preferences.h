#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Preferences { class PreferencesService; }

namespace AqualinkAutomate::HTTP
{
	inline constexpr char PREFERENCES_ROUTE_URL[] = "/api/preferences";

	// GET returns the current preferences; PUT validates + applies a (partial)
	// preferences document and persists it. 400 on a bad value, 503 if the
	// preferences service is unavailable.
	class WebRoute_Preferences : public Interfaces::IWebRoute<PREFERENCES_ROUTE_URL>
	{
	public:
		explicit WebRoute_Preferences(std::shared_ptr<Preferences::PreferencesService> service);
		~WebRoute_Preferences() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::PREFS_SELF };
		}

	private:
		std::shared_ptr<Preferences::PreferencesService> m_Service;
	};

}
// namespace AqualinkAutomate::HTTP
