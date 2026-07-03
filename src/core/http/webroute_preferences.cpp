#include <array>
#include <source_location>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "auth/entitlement_vocabulary.h"
#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/responses/response_503.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/server/routing/routing.h"
#include "http/webroute_preferences.h"
#include "preferences/preferences_service.h"
#include "preferences/user_preferences_store.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// The per-user field vocabulary (D7) — the split boundary between the
		// caller's own slice and the global system/admin preferences.
		constexpr std::array<std::string_view, 4> PER_USER_KEYS{
			"temperature_units",
			"theme",
			"accent",
			"chemistry_bands"
		};

		bool IsPerUserKey(std::string_view key)
		{
			return std::ranges::find(PER_USER_KEYS, key) != PER_USER_KEYS.end();
		}

		// Ensure the per-user display fields are always present in a GET view so
		// a fresh client has values to render: theme/accent from built-ins,
		// chemistry_bands from the legacy ui.chemistryBands blob when present.
		void EnsurePerUserDefaults(nlohmann::json& view)
		{
			if (!view.contains("theme")) { view["theme"] = "system"; }
			if (!view.contains("accent")) { view["accent"] = "teal"; }

			if (!view.contains("chemistry_bands"))
			{
				view["chemistry_bands"] = (view.contains("ui") && view["ui"].is_object())
					? view["ui"].value("chemistryBands", nlohmann::json::object())
					: nlohmann::json::object();
			}
		}
	}
	// anonymous namespace

	WebRoute_Preferences::WebRoute_Preferences(std::shared_ptr<Preferences::PreferencesService> service, std::shared_ptr<Preferences::UserPreferencesStore> user_prefs) :
		m_Service(std::move(service)),
		m_UserPrefs(std::move(user_prefs))
	{
	}

	HTTP::Response WebRoute_Preferences::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Preferences::OnRequest", std::source_location::current());

		if (!m_Service)
		{
			return HTTP::Responses::Response_503(req);
		}

		const auto& subject = Routing::CurrentSubject();

		// The per-user path is active only when the identity system is on, a
		// per-user store is wired, and the caller is authenticated.  Otherwise
		// the route behaves exactly as it did historically (all global).
		const bool per_user_active = Routing::GetSecurityConfig().AuthModeEnabled && (nullptr != m_UserPrefs) && subject.Authenticated;

		const auto merged_view = [&]()
		{
			nlohmann::json view = m_Service->ToJson();
			EnsurePerUserDefaults(view);

			if (per_user_active)
			{
				// Overlay the caller's overrides on the LIVE global values.
				for (const auto& [key, value] : m_UserPrefs->Overrides(subject.Id).items())
				{
					view[key] = value;
				}
			}

			return view;
		};

		switch (req.method())
		{
		case HTTP::Verbs::get:
			return MakeJsonResponse(req, HTTP::Status::ok, merged_view().dump());

		case HTTP::Verbs::put:
		{
			auto json = nlohmann::json::parse(req.body(), nullptr, /*allow_exceptions=*/false);
			if (!json.is_object())
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
			}

			if (!per_user_active)
			{
				// Auth off (or no per-user store): historical behaviour — the
				// whole document goes to the global service, which picks out the
				// fields it knows (units, ui, alert, ...) and ignores the rest.
				std::string error;
				std::string error_code;
				if (!m_Service->ApplyJson(json, error, error_code))
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, error_code.empty() ? "invalid_preferences" : error_code, error);
				}
				return MakeJsonResponse(req, HTTP::Status::ok, merged_view().dump());
			}

			// Split the document: per-user fields go to the caller's slice; any
			// system/admin field requires the system.admin entitlement.
			nlohmann::json user_fields = nlohmann::json::object();
			nlohmann::json system_fields = nlohmann::json::object();

			for (const auto& [key, value] : json.items())
			{
				(IsPerUserKey(key) ? user_fields : system_fields)[key] = value;
			}

			if (!system_fields.empty())
			{
				if (!subject.Entitlements.Permits(Auth::Vocabulary::SYSTEM_ADMIN))
				{
					return MakeErrorResponse(req, HTTP::Status::forbidden, "admin_required", "system preferences require administrator access");
				}

				std::string error;
				std::string error_code;
				if (!m_Service->ApplyJson(system_fields, error, error_code))
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, error_code.empty() ? "invalid_preferences" : error_code, error);
				}
			}

			if (!user_fields.empty())
			{
				std::string error;
				if (!m_UserPrefs->Apply(subject.Id, user_fields, error))
				{
					return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_preferences", error);
				}
			}

			return MakeJsonResponse(req, HTTP::Status::ok, merged_view().dump());
		}

		default:
			return HTTP::Responses::Response_405(req);
		}
	}

}
// namespace AqualinkAutomate::HTTP
