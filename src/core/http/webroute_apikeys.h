#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char APIKEYS_ROUTE_URL[] = "/api/apikeys";

	// API-key collection (docs/auth-redesign.md §7): GET lists every key
	// (id, label, entitlements, expiry, last-used, revoked — NEVER the secret
	// or its digest); POST { label, entitlements[], expiry_unix? } creates one
	// (201).  The response carries the ONE-TIME secret plus a warning field:
	// only the digest is persisted, so the secret can never be shown again.
	// Entitlements are validated like groups' (400 listing the rejects).
	class WebRoute_ApiKeys : public Interfaces::IWebRoute<APIKEYS_ROUTE_URL>
	{
	public:
		WebRoute_ApiKeys(Auth::ApiKeyStore& keys, Auth::AuditLog& audit);

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
