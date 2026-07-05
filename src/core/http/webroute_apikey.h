#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char APIKEY_ROUTE_URL[] = "/api/apikeys/{key_id}";

	// Individual API key (docs/auth-redesign.md §7): DELETE revokes it — the
	// key stays on file (revoked, for hygiene audits) but authenticates
	// nothing from the next request on.  Unknown id -> 404.  Audited.
	class WebRoute_ApiKey : public Interfaces::IWebRoute<APIKEY_ROUTE_URL>
	{
	public:
		WebRoute_ApiKey(Auth::ApiKeyStore& keys, Auth::AuditLog& audit);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		Auth::ApiKeyStore& m_Keys;
		Auth::AuditLog& m_Audit;
	};

}
// namespace AqualinkAutomate::HTTP
