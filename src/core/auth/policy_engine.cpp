#include "auth/entitlement_vocabulary.h"
#include "auth/policy_engine.h"

namespace AqualinkAutomate::Auth
{

	Decision PolicyEngine::Decide(const Subject& subject, std::string_view action, const ResourceRef& resource, const Environment& environment)
	{
		using enum Decision;

		if (!environment.AuthEnabled)
		{
			// Posture auth-OFF: the identity system is disabled and every request
			// runs as the historical root-anonymous subject.
			return Permit;
		}

		if (subject.Entitlements.Permits(Vocabulary::SYSTEM_ADMIN))
		{
			// Superuser entitlement short-circuits every action.
			return Permit;
		}

		if (subject.Entitlements.Permits(action, resource.Id))
		{
			return Permit;
		}

		return Deny;
	}

}
// namespace AqualinkAutomate::Auth
