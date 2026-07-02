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

		// Step 1: our own session JWT, cross-checked against the user store.
		std::optional<Subject> ResolveJwt(const SubjectResolverDeps& deps, std::string_view token)
		{
			if (nullptr == deps.Codec)
			{
				return std::nullopt;
			}

			const auto claims = deps.Codec->Verify(std::string{ token });

			if (!claims.has_value())
			{
				return std::nullopt;
			}

			Subject subject;
			subject.Id = claims->Subject;
			subject.Authenticated = true;
			subject.Provider = claims->Provider;
			subject.Groups = claims->Groups;
			subject.TokenVersion = claims->TokenVersion;

			if (nullptr != deps.Users)
			{
				// D15 immediate propagation: logout-all/disable/entitlement change
				// bumped the store's tokver, so this (validly signed) token is stale.
				const auto user = deps.Users->FindById(claims->Subject);

				if (!user.has_value() || user->Disabled || (user->TokenVersion != claims->TokenVersion))
				{
					return std::nullopt;
				}

				// The store is authoritative for group membership even when the
				// token carried its own snapshot.
				subject.Groups = user->Groups;

				if (claims->EntitlementsInToken)
				{
					subject.Entitlements = EntitlementSet::Parse(claims->Entitlements);
				}
				else
				{
					subject.Entitlements = deps.Groups->ResolveEffectiveEntitlements(user->DirectEntitlements, user->Groups);
				}

				return subject;
			}

			// No user store wired (substrate tests): trust the verified claims.
			if (claims->EntitlementsInToken)
			{
				subject.Entitlements = EntitlementSet::Parse(claims->Entitlements);
			}
			else
			{
				subject.Entitlements = deps.Groups->ResolveEffectiveEntitlements({}, subject.Groups);
			}

			return subject;
		}

		// Step 2: API key (aak_... or the folded-in legacy token), by digest.
		std::optional<Subject> ResolveApiKey(const SubjectResolverDeps& deps, std::string_view token)
		{
			if (nullptr == deps.ApiKeys)
			{
				return std::nullopt;
			}

			const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(deps.Now().time_since_epoch()).count();

			const auto key = deps.ApiKeys->Authenticate(token, now_unix);

			if (!key.has_value())
			{
				return std::nullopt;
			}

			Subject subject;
			subject.Id = key->Id;
			subject.Authenticated = true;
			subject.Provider = SubjectProvider::ApiKey;
			subject.Entitlements = key->Entitlements;

			return subject;
		}
	}
	// anonymous namespace

	HTTP::Routing::SubjectResolver MakeSubjectResolver(SubjectResolverDeps deps)
	{
		return [deps = std::move(deps)](const HTTP::Request& req, [[maybe_unused]] bool is_websocket_upgrade) -> Subject
		{
			const auto token = BearerToken(req);

			if (!token.empty())
			{
				if (auto subject = ResolveJwt(deps, token); subject.has_value())
				{
					return *subject;
				}

				if (auto subject = ResolveApiKey(deps, token); subject.has_value())
				{
					return *subject;
				}
			}

			// No credential, or nothing verifiable: anonymous == Guest scope;
			// the PolicyEngine answers 401 for anything beyond it.
			return AnonymousGuest(*deps.Groups);
		};
	}

	HTTP::Routing::SubjectResolver MakeSubjectResolver(std::shared_ptr<GroupRegistry> groups, std::shared_ptr<JwtCodec> codec)
	{
		return MakeSubjectResolver(SubjectResolverDeps{ .Groups = std::move(groups), .Codec = std::move(codec) });
	}

}
// namespace AqualinkAutomate::Auth
