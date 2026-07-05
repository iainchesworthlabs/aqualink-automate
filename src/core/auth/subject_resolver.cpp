#include <string>
#include <string_view>

#include <boost/beast/http/field.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/subject_resolver.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::string_view BEARER_PREFIX{ "Bearer " };
		constexpr std::string_view WS_BEARER_ENTRY_PREFIX{ "bearer." };

		// Browsers cannot set an Authorization header on a WebSocket upgrade, so
		// the UI offers the token as a `bearer.<token>` entry in the
		// Sec-WebSocket-Protocol header instead (e.g. "aqualink, bearer.<token>").
		// Mirrors routing.cpp's WebSocketSubprotocolTokenMatches parsing, but
		// extracts the token rather than comparing it to one expected value,
		// since the JWT/API-key path has no single shared secret to compare
		// against.
		std::string_view WebSocketSubprotocolBearerToken(const HTTP::Request& req)
		{
			const auto it = req.find(boost::beast::http::field::sec_websocket_protocol);

			if (req.end() == it)
			{
				return {};
			}

			const std::string_view header{ it->value().data(), it->value().size() };

			std::size_t pos = 0;
			while (pos <= header.size())
			{
				const std::size_t comma = header.find(',', pos);
				std::string_view entry = (comma == std::string_view::npos)
					? header.substr(pos)
					: header.substr(pos, comma - pos);

				while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) { entry.remove_prefix(1); }
				while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) { entry.remove_suffix(1); }

				if (entry.starts_with(WS_BEARER_ENTRY_PREFIX))
				{
					return entry.substr(WS_BEARER_ENTRY_PREFIX.size());
				}

				if (comma == std::string_view::npos) { break; }
				pos = comma + 1;
			}

			return {};
		}

		std::string_view BearerToken(const HTTP::Request& req, bool is_websocket_upgrade)
		{
			if (is_websocket_upgrade)
			{
				return WebSocketSubprotocolBearerToken(req);
			}

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

		// prefs.self is IMPLICIT for authenticated HUMAN subjects (see
		// entitlement_vocabulary.h): every logged-in user may manage their own
		// preferences, password and sessions without an explicit grant.  It is
		// NOT granted to API keys (machine credentials have no "self") nor to
		// the anonymous subject.
		void GrantImplicitSelfEntitlements(Subject& subject)
		{
			subject.Entitlements.Add(Entitlement{ std::string{ Vocabulary::PREFS_SELF } });
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

			// Kiosk PIN sessions are NOT user records: validate them against the
			// kiosk store (still enabled + tokver current, mirroring the user
			// tokver check) and resolve entitlements from the token's group
			// snapshot.  No prefs.self — a shared terminal has no "self".
			if (SubjectProvider::KioskPin == claims->Provider)
			{
				if ((nullptr == deps.Kiosk) || !deps.Kiosk->Enabled() || (deps.Kiosk->TokenVersion() != claims->TokenVersion))
				{
					return std::nullopt;
				}

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

			if (nullptr != deps.Users)
			{
				// D15 immediate propagation: logout-all/disable/entitlement change
				// bumped the store's tokver, so this (validly signed) token is stale.
				const auto user = deps.Users->FindById(claims->Subject);

				if (!user.has_value() || user->Disabled || (user->TokenVersion != claims->TokenVersion))
				{
					return std::nullopt;
				}

				// The store is authoritative for group membership and the
				// username even when the token carried its own snapshot.
				subject.Groups = user->Groups;
				subject.Username = user->Username;

				if (claims->EntitlementsInToken)
				{
					subject.Entitlements = EntitlementSet::Parse(claims->Entitlements);
				}
				else
				{
					subject.Entitlements = deps.Groups->ResolveEffectiveEntitlements(user->DirectEntitlements, user->Groups);
				}

				GrantImplicitSelfEntitlements(subject);

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

			GrantImplicitSelfEntitlements(subject);

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
			subject.Username = key->Label;
			subject.Authenticated = true;
			subject.Provider = SubjectProvider::ApiKey;
			subject.Entitlements = key->Entitlements;

			return subject;
		}
	}
	// anonymous namespace

	HTTP::Routing::SubjectResolver MakeSubjectResolver(SubjectResolverDeps deps)
	{
		return [deps = std::move(deps)](const HTTP::Request& req, bool is_websocket_upgrade) -> Subject
		{
			if (const auto token = BearerToken(req, is_websocket_upgrade); !token.empty())
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
