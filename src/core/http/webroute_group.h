#pragma once

#include <boost/beast/http/verb.hpp>

#include "auth/audit_log.h"
#include "auth/entitlement_vocabulary.h"
#include "auth/group_store.h"
#include "auth/user_store.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char GROUP_ROUTE_URL[] = "/api/groups/{group_name}";

	// Individual group (docs/auth-redesign.md §4): DELETE removes a
	// (non-built-in) group — built-ins (Everyone/Guest/Administrators) answer
	// 409, an unknown name 404.  Memberships referencing the deleted group
	// degrade safely (no entitlements), so every member's TokenVersion is
	// bumped for D15 immediate propagation.
	class WebRoute_Group : public Interfaces::IWebRoute<GROUP_ROUTE_URL>
	{
	public:
		WebRoute_Group(Auth::GroupStore& groups, Auth::UserStore& users, Auth::AuditLog& audit);

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
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
