#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "auth/session_service.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auth
{

	namespace
	{
		std::string LowercaseKey(std::string_view username)
		{
			std::string key;
			key.reserve(username.size());

			for (const unsigned char ch : username)
			{
				key.push_back(static_cast<char>(std::tolower(ch)));
			}

			return key;
		}

		AuditEvent MakeAuthEvent(std::string_view action, std::string_view subject_id, std::string_view decision, std::string_view peer_ip, std::string_view detail)
		{
			AuditEvent event;
			event.SubjectId = std::string{ subject_id };
			event.Provider = std::string{ magic_enum::enum_name(SubjectProvider::Local) };
			event.Action = std::string{ action };
			event.Decision = std::string{ decision };
			event.PeerIp = std::string{ peer_ip };
			event.Detail = std::string{ detail };
			return event;
		}
	}
	// anonymous namespace

	SessionService::SessionService(std::shared_ptr<UserStore> users,
		std::shared_ptr<GroupStore> groups,
		std::shared_ptr<SessionStore> sessions,
		std::shared_ptr<JwtCodec> codec,
		Utility::OffloadPool& offload,
		AuditLog& audit,
		Config config) :
		m_Users(std::move(users)),
		m_Groups(std::move(groups)),
		m_Sessions(std::move(sessions)),
		m_Codec(std::move(codec)),
		m_Offload(offload),
		m_Audit(audit),
		m_Config(std::move(config)),
		// Same cost parameters as real hashes, so an unknown-username failure
		// takes as long as a wrong-password failure (no account enumeration).
		m_DecoyHash(PasswordHasher::Hash("decoy-password-for-timing-equalisation", m_Config.HashParams))
	{
	}

	void SessionService::Login(std::string username, std::string password, std::string peer_ip, std::string user_agent, boost::asio::any_io_executor executor, LoginCompletion completion)
	{
		const auto username_key = LowercaseKey(username);

		if (IsLockedOut(username_key))
		{
			m_Audit.Record(MakeAuthEvent("auth.login", username, "failure", peer_ip, "account locked out"));

			LoginResult result;
			result.LockedOut = true;
			result.Error = "Too many failed attempts; try again later";

			completion(std::move(result));
			return;
		}

		const auto user = m_Users->FindByUsername(username);
		const bool usable = user.has_value() && !user->Disabled;

		// The verify ALWAYS runs (against the decoy when the account is not
		// usable) so timing does not reveal whether the username exists.
		const std::string hash = usable ? user->PasswordHash : m_DecoyHash;

		m_Offload.Run(std::move(executor),
			[password = std::move(password), hash]() { return PasswordHasher::Verify(password, hash); },
			[this, usable, user, username = std::move(username), username_key, peer_ip = std::move(peer_ip), user_agent = std::move(user_agent), completion = std::move(completion)](bool verified) mutable
			{
				if (!usable || !verified)
				{
					RecordFailure(username_key);
					m_Audit.Record(MakeAuthEvent("auth.login", username, "failure", peer_ip, usable ? "wrong password" : "unknown or disabled account"));

					LoginResult result;
					result.Error = "Invalid username or password";

					completion(std::move(result));
					return;
				}

				m_Failures.erase(username_key);

				FinishLogin(verified, *user, peer_ip, std::move(user_agent), completion);
			});
	}

	void SessionService::FinishLogin(bool /*verified*/, const UserRecord& user, const std::string& peer_ip, std::string user_agent, const LoginCompletion& completion)
	{
		const auto now_unix = NowUnix();
		const auto refresh_expiry = now_unix + m_Config.RefreshTtl.count();

		LoginResult result;
		result.Success = true;
		result.AccessToken = MintAccessToken(user);
		result.RefreshToken = m_Sessions->Create(user.Id, refresh_expiry, std::move(user_agent), peer_ip, now_unix, result.SessionId);

		m_Audit.Record(MakeAuthEvent("auth.login", user.Id, "success", peer_ip, std::format("session {}", result.SessionId)));

		completion(std::move(result));
	}

	SessionService::RefreshResult SessionService::Refresh(const std::string& refresh_token, std::string_view peer_ip)
	{
		RefreshResult result;

		const auto rotation = m_Sessions->Rotate(refresh_token, NowUnix());

		if (rotation.ReuseDetected)
		{
			// Stolen-token tripwire: the session is already gone; make the
			// incident loud on the audit trail.
			m_Audit.Record(MakeAuthEvent("auth.refresh", rotation.UserId, "failure", peer_ip, std::format("rotated-out refresh token replayed; session {} revoked", rotation.SessionId)));

			result.Error = "Session revoked";
			return result;
		}

		if (!rotation.Success)
		{
			result.Error = "Invalid or expired session";
			return result;
		}

		const auto user = m_Users->FindById(rotation.UserId);

		if (!user.has_value() || user->Disabled)
		{
			// The account went away/disabled while the session lived on.
			m_Sessions->RevokeById(rotation.SessionId);
			m_Audit.Record(MakeAuthEvent("auth.refresh", rotation.UserId, "failure", peer_ip, "account missing or disabled"));

			result.Error = "Invalid or expired session";
			return result;
		}

		result.Success = true;
		result.AccessToken = MintAccessToken(*user);
		result.RefreshToken = rotation.NewRefreshSecret;

		return result;
	}

	bool SessionService::Logout(const std::string& refresh_token, std::string_view peer_ip)
	{
		const bool revoked = m_Sessions->RevokeByRefresh(refresh_token);

		if (revoked)
		{
			m_Audit.Record(MakeAuthEvent("auth.logout", "-", "success", peer_ip, ""));
		}

		return revoked;
	}

	std::size_t SessionService::LogoutAll(std::string_view user_id, std::string_view peer_ip)
	{
		const auto dropped = m_Sessions->RevokeAllForUser(user_id);

		// Bump tokver even when no refresh sessions existed: outstanding ACCESS
		// tokens must die too (D15 immediate propagation).
		m_Users->BumpTokenVersion(user_id);

		m_Audit.Record(MakeAuthEvent("auth.logout_all", user_id, "success", peer_ip, std::format("{} sessions revoked", dropped)));

		return dropped;
	}

	bool SessionService::DisableUser(std::string_view user_id, std::string& error)
	{
		auto user = m_Users->FindById(user_id);

		if (!user.has_value())
		{
			error = "User not found";
			return false;
		}

		user->Disabled = true;

		if (!m_Users->Update(*user, m_Groups->Registry(), error))
		{
			return false;   // e.g. last-admin protection.
		}

		m_Users->BumpTokenVersion(user_id);
		m_Sessions->RevokeAllForUser(user_id);

		m_Audit.Record(MakeAuthEvent("auth.user_disabled", user_id, "success", "", ""));

		return true;
	}

	std::string SessionService::MintAccessToken(const UserRecord& user) const
	{
		// Re-read the record so the tokver claim reflects any bump between the
		// caller's snapshot and now.
		const auto current = m_Users->FindById(user.Id);
		const auto& record = current.has_value() ? *current : user;

		TokenClaims claims;
		claims.Subject = record.Id;
		claims.Provider = SubjectProvider::Local;
		claims.TokenVersion = record.TokenVersion;
		claims.Groups = record.Groups;
		claims.Entitlements = m_Groups->Registry().ResolveEffectiveEntitlements(record.DirectEntitlements, record.Groups).ToStrings();

		return m_Codec->Sign(claims);
	}

	void SessionService::RecordFailure(const std::string& username_key)
	{
		auto& entry = m_Failures[username_key];

		if (++entry.Count >= m_Config.MaxFailuresPerAccount)
		{
			entry.LockedUntil = m_Config.Now() + m_Config.AccountLockout;
			entry.Count = 0;

			m_Audit.Record(MakeAuthEvent("auth.lockout", username_key, "failure", "", std::format("account locked for {}s after repeated failures", m_Config.AccountLockout.count())));
		}
	}

	bool SessionService::IsLockedOut(const std::string& username_key)
	{
		const auto it = m_Failures.find(username_key);

		return (it != m_Failures.end()) && (m_Config.Now() < it->second.LockedUntil);
	}

	std::int64_t SessionService::NowUnix() const
	{
		return std::chrono::duration_cast<std::chrono::seconds>(m_Config.Now().time_since_epoch()).count();
	}

}
// namespace AqualinkAutomate::Auth
