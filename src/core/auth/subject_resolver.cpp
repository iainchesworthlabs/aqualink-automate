#include <string>
#include <string_view>

#include <boost/beast/http/field.hpp>

#include "auth/subject_resolver.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::string_view BEARER_PREFIX{ "Bearer " };

		std::string_view BearerToken(const HTTP::Request& req)
		{
			const auto it = req.find(boost::beast::http::field::authorization);

			if (req.end() == it)
			{
				return {};
			}

			const std::string_view header{ it->value().data(), it->value().size() };

			if (!header.starts_with(BEARER_PREFIX))
			{
				return {};
			}

			return header.substr(BEARER_PREFIX.size());
		}

		Subject AnonymousGuest(const GroupRegistry& groups)
		{
			Subject subject = Subject::Anonymous();

			subject.Groups = { std::string{ BuiltInGroups::GUEST } };
			subject.Entitlements = groups.ResolveEffectiveEntitlements({}, subject.Groups);

			return subject;
		}
	}
	// anonymous namespace

	HTTP::Routing::SubjectResolver MakeSubjectResolver(std::shared_ptr<GroupRegistry> groups, std::shared_ptr<JwtCodec> codec)
	{
		return [groups, codec](const HTTP::Request& req, [[maybe_unused]] bool is_websocket_upgrade) -> Subject
		{
			const auto token = BearerToken(req);

			if (token.empty() || (nullptr == codec))
			{
				return AnonymousGuest(*groups);
			}

			const auto claims = codec->Verify(std::string{ token });

			if (!claims.has_value())
			{
				// Unverifiable credentials degrade to the anonymous subject; the
				// PolicyEngine then answers 401 for anything beyond guest scope.
				return AnonymousGuest(*groups);
			}

			Subject subject;
			subject.Id = claims->Subject;
			subject.Authenticated = true;
			subject.Provider = claims->Provider;
			subject.Groups = claims->Groups;
			subject.TokenVersion = claims->TokenVersion;

			if (claims->EntitlementsInToken)
			{
				subject.Entitlements = EntitlementSet::Parse(claims->Entitlements);
			}
			else
			{
				// Size-overflow rule: the token carried groups only; re-resolve the
				// effective set from the server-side stores.
				subject.Entitlements = groups->ResolveEffectiveEntitlements({}, subject.Groups);
			}

			return subject;
		};
	}

}
// namespace AqualinkAutomate::Auth
