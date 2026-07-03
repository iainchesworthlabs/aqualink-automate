#include <cstdint>
#include <source_location>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/routing/routing.h"
#include "http/webroute_admin_helpers.h"
#include "http/webroute_apikeys.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// The public JSON shape of a key record: NEVER the secret's digest.
		nlohmann::json ApiKeyToJson(const Auth::ApiKeyRecord& key)
		{
			return
			{
				{ "id", key.Id },
				{ "label", key.Label },
				{ "entitlements", key.Entitlements.ToStrings() },
				{ "expiry_unix", key.ExpiryUnix },
				{ "last_used_unix", key.LastUsedUnix },
				{ "revoked", key.Revoked }
			};
		}
	}
	// anonymous namespace

	WebRoute_ApiKeys::WebRoute_ApiKeys(Auth::ApiKeyStore& keys, Auth::AuditLog& audit) :
		m_Keys(keys),
		m_Audit(audit)
	{
	}

	HTTP::Response WebRoute_ApiKeys::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_ApiKeys::OnRequest", std::source_location::current());

		if (boost::beast::http::verb::get == req.method())
		{
			nlohmann::json keys = nlohmann::json::array();

			for (const auto& key : m_Keys.All())
			{
				keys.push_back(ApiKeyToJson(key));
			}

			return MakeJsonResponse(req, HTTP::Status::ok, keys.dump());
		}

		if (boost::beast::http::verb::post != req.method())
		{
			return MakeJsonResponse(req, HTTP::Status::method_not_allowed, nlohmann::json{ { "error", "GET or POST required" } }.dump());
		}

		const auto body = nlohmann::json::parse(req.body(), nullptr, false);

		if (body.is_discarded() || !body.contains("label") || !body["label"].is_string() || !body.contains("entitlements"))
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected JSON body with label and entitlements" } }.dump());
		}

		const auto label = body["label"].get<std::string>();

		if (label.empty())
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Label is required" } }.dump());
		}

		std::string error;
		auto entitlements = ParseEntitlementsField(body["entitlements"], error);

		if (!entitlements.has_value())
		{
			return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", error } }.dump());
		}

		std::int64_t expiry_unix{ 0 };

		if (body.contains("expiry_unix"))
		{
			if (!body["expiry_unix"].is_number_integer())
			{
				return MakeJsonResponse(req, HTTP::Status::bad_request, nlohmann::json{ { "error", "Expected expiry_unix to be a unix timestamp" } }.dump());
			}

			expiry_unix = body["expiry_unix"].get<std::int64_t>();
		}

		std::string key_id;
		const auto secret = m_Keys.Create(label, std::move(*entitlements), expiry_unix, key_id);

		m_Audit.Record(MakeAdminAuditEvent(Routing::CurrentSubject().Id, "auth.apikey_created", "apikey", key_id, Routing::CurrentPeerIp(), label));

		const auto created = m_Keys.FindById(key_id);

		nlohmann::json response = created.has_value() ? ApiKeyToJson(*created) : nlohmann::json{ { "id", key_id }, { "label", label } };

		// Shown ONCE: only the digest is persisted (§7).
		response["secret"] = secret;
		response["warning"] = "Store this secret now; it cannot be retrieved again";

		return MakeJsonResponse(req, HTTP::Status::created, response.dump());
	}

}
// namespace AqualinkAutomate::HTTP
