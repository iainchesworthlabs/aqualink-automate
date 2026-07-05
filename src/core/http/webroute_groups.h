#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/group_store.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char GROUPS_ROUTE_URL[] = "/api/groups";

	// Group collection (docs/auth-redesign.md §4, D10): GET lists every group
	// (name, entitlements, built_in); POST { name, entitlements[] } upserts one
	// — every entitlement must parse AND carry a known vocabulary action (400
	// listing the rejects otherwise; a typo must not create a dead grant).
	//
	// Propagation (D15): changing a group's entitlements bumps the TokenVersion
	// of EVERY member user, so the grant change takes effect within a request.
	class WebRoute_Groups : public Interfaces::IWebRoute<GROUPS_ROUTE_URL>
	{
	public:
		WebRoute_Groups(Auth::GroupStore& groups, Auth::UserStore& users, Auth::AuditLog& audit);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SYSTEM_ADMIN };
		}

	private:
		Auth::GroupStore& m_Groups;
		Auth::UserStore& m_Users;
		Auth::AuditLog& m_Audit;
	};

}
// namespace AqualinkAutomate::HTTP
