#include <chrono>
#include <format>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "auth/kiosk_service.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		AuditEvent MakeKioskEvent(std::string_view action, std::string_view decision, std::string_view peer_ip, std::string_view actor_id, std::string_view detail)
		{
			AuditEvent event;
			event.SubjectId = actor_id.empty() ? std::string{ KioskService::SubjectId } : std::string{ actor_id };
			event.Provider = std::string{ magic_enum::enum_name(SubjectProvider::KioskPin) };
			event.Action = std::string{ action };
			event.Decision = std::string{ decision };
			event.PeerIp = std::string{ peer_ip };
			event.Detail = std::string{ detail };
			return event;
		}
	}
	// anonymous namespace

	KioskService::KioskService(std::shared_ptr<KioskStore> kiosk,
		std::shared_ptr<GroupStore> groups,
		std::shared_ptr<SessionStore> sessions,
		std::shared_ptr<JwtCodec> codec,
		Utility::OffloadPool& offload,
		AuditLog& audit,
		Config config) :
		m_Kiosk(std::move(kiosk)),
		m_Groups(std::move(groups)),
		m_Sessions(std::move(sessions)),
		m_Codec(std::move(codec)),
		m_Offload(offload),
		m_Audit(audit),
		m_Config(std::move(config)),
		// Same cost as a real PIN hash, so a login attempt against a kiosk with
		// NO PIN configured takes as long as a wrong-PIN attempt (no oracle for
		// "is a PIN even set?").
		m_DecoyHash(PasswordHasher::Hash("decoy-pin-for-timing-equalisation", m_Config.HashParams))
	{
	}

	bool KioskService::Enabled() const
	{
		return m_Kiosk->Enabled();
	}

	std::string KioskService::TargetGroup() const
	{
		return m_Kiosk->TargetGroup();
	}

	void KioskService::SetPin(std::string raw_pin, std::string target_group, std::string actor_id, std::string peer_ip, boost::asio::any_io_executor executor, SetPinCompletion completion)
	{
		if (raw_pin.size() < m_Config.MinPinLength)
		{
			m_Audit.Record(MakeKioskEvent("auth.kiosk_configure", "failure", peer_ip, actor_id, "PIN too short"));
			completion(SetPinResult{ .Success = false, .Error = std::format("PIN must be at least {} characters", m_Config.MinPinLength) });
			return;
		}

		// The target group must exist (built-in Guest/custom groups are valid
		// targets; Everyone is pointless but harmless).  Reject unknowns so an
		// admin cannot strand the kiosk in a non-existent group.
		if (!m_Groups->Registry().Find(target_group).has_value())
		{
			m_Audit.Record(MakeKioskEvent("auth.kiosk_configure", "failure", peer_ip, actor_id, "unknown target group"));
			completion(SetPinResult{ .Success = false, .Error = "Unknown target group" });
			return;
		}

		m_Offload.Run(std::move(executor),
			[raw_pin = std::move(raw_pin), params = m_Config.HashParams]() { return PasswordHasher::Hash(raw_pin, params); },
			[this, target_group = std::move(target_group), actor_id = std::move(actor_id), peer_ip = std::move(peer_ip), completion = std::move(completion)](std::string pin_hash) mutable
			{
				m_Kiosk->Configure(std::move(pin_hash), target_group);

				// A reconfigure supersedes any live kiosk session; the tokver
				// bump already invalidated their access tokens, so drop the
				// refresh sessions too.
				m_Sessions->RevokeAllForUser(SubjectId);

				m_Audit.Record(MakeKioskEvent("auth.kiosk_configure", "success", peer_ip, actor_id, std::format("target group {}", target_group)));

				completion(SetPinResult{ .Success = true });
			});
	}

	void KioskService::ClearPin(std::string_view actor_id, std::string_view peer_ip)
	{
		m_Kiosk->Disable();
		m_Sessions->RevokeAllForUser(SubjectId);
		m_Audit.Record(MakeKioskEvent("auth.kiosk_disable", "success", peer_ip, actor_id, ""));
	}

	void KioskService::LoginWithPin(std::string raw_pin, std::string peer_ip, std::string user_agent, boost::asio::any_io_executor executor, LoginCompletion completion)
	{
		if (IsLockedOut())
		{
			m_Audit.Record(MakeKioskEvent("auth.kiosk_login", "failure", peer_ip, {}, "kiosk PIN locked out"));

			LoginResult result;
			result.LockedOut = true;
			result.Error = "Too many failed attempts; try again later";

			completion(std::move(result));
			return;
		}

		const bool usable = m_Kiosk->Enabled() && !m_Kiosk->PinHash().empty();

		// Verify ALWAYS runs (against the decoy when no PIN is configured) so
		// timing does not reveal whether the kiosk PIN is enabled.
		const std::string hash = usable ? m_Kiosk->PinHash() : m_DecoyHash;

		m_Offload.Run(std::move(executor),
			[raw_pin = std::move(raw_pin), hash]() { return PasswordHasher::Verify(raw_pin, hash); },
			[this, usable, peer_ip = std::move(peer_ip), user_agent = std::move(user_agent), completion = std::move(completion)](bool verified) mutable
			{
				if (!usable || !verified)
				{
					RecordFailure();
					m_Audit.Record(MakeKioskEvent("auth.kiosk_login", "failure", peer_ip, {}, usable ? "wrong PIN" : "kiosk PIN not enabled"));

					LoginResult result;
					result.Error = "Incorrect PIN";

					completion(std::move(result));
					return;
				}

				m_Failure = FailureEntry{};

				const auto now_unix = NowUnix();
				const auto refresh_expiry = now_unix + m_Config.RefreshTtl.count();

				LoginResult result;
				result.Success = true;
				result.AccessToken = MintKioskToken();
				result.RefreshToken = m_Sessions->Create(std::string{ SubjectId }, refresh_expiry, std::move(user_agent), peer_ip, now_unix, result.SessionId);

				m_Audit.Record(MakeKioskEvent("auth.kiosk_login", "success", peer_ip, {}, std::format("session {}", result.SessionId)));

				completion(std::move(result));
			});
	}

	std::string KioskService::MintKioskToken() const
	{
		const std::vector<std::string> groups{ m_Kiosk->TargetGroup() };

		TokenClaims claims;
		claims.Subject = std::string{ SubjectId };
		claims.Provider = SubjectProvider::KioskPin;
		claims.TokenVersion = m_Kiosk->TokenVersion();
		claims.Groups = groups;
		claims.Entitlements = m_Groups->Registry().ResolveEffectiveEntitlements({}, groups).ToStrings();

		return m_Codec->Sign(claims);
	}

	void KioskService::RecordFailure()
	{
		auto& entry = m_Failure;
		++entry.Count;

		if (entry.Count >= m_Config.MaxFailures)
		{
			entry.LockedUntil = m_Config.Now() + m_Config.Lockout;
		}
	}

	bool KioskService::IsLockedOut()
	{
		auto& entry = m_Failure;

		if (entry.LockedUntil == std::chrono::system_clock::time_point{})
		{
			return false;
		}

		if (m_Config.Now() >= entry.LockedUntil)
		{
			// The window elapsed — reset so a fresh burst is measured cleanly.
			entry = FailureEntry{};
			return false;
		}

		return true;
	}

	std::int64_t KioskService::NowUnix() const
	{
		return std::chrono::duration_cast<std::chrono::seconds>(m_Config.Now().time_since_epoch()).count();
	}

}
// namespace AqualinkAutomate::Auth
