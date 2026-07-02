#include <source_location>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "http/server/routing/routing.h"
#include "http/server/server_fields.h"
#include "http/webroute_auth_me.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	HTTP::Response WebRoute_AuthMe::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_AuthMe::OnRequest", std::source_location::current());

		const auto& subject = Routing::CurrentSubject();
		const bool auth_mode = Routing::GetSecurityConfig().AuthModeEnabled;

		nlohmann::json body;
		body["posture"] = auth_mode ? "enabled" : "disabled";
		body["setup_required"] = auth_mode && m_SetupRequired && m_SetupRequired();
		body["kiosk_enabled"] = auth_mode && m_KioskEnabled && m_KioskEnabled();
		body["id"] = subject.Id;
		body["authenticated"] = subject.Authenticated;
		body["provider"] = magic_enum::enum_name(subject.Provider);
		body["groups"] = subject.Groups;

		// Sorted + deterministic (the same shape as the JWT `ent` claim).  With
		// auth-mode disabled the subject is root-anonymous: entitlements are not
		// enumerated because everything is permitted by posture.
		body["entitlements"] = subject.Entitlements.ToStrings();

		HTTP::Response resp{ HTTP::Status::ok, req.version() };
		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
		resp.keep_alive(req.keep_alive());
		resp.body() = body.dump();
		resp.prepare_payload();

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
