#include <algorithm>
#include <iterator>
#include <stdexcept>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/rand.h>

#include "auth/auth_store_file.h"
#include "auth/session_store.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr std::string_view REFRESH_PREFIX{ "art_" };
		constexpr std::size_t REFRESH_RANDOM_BYTES{ 32 };

		nlohmann::json ToJson(const SessionRecord& session)
		{
			return {
				{ "id", session.Id },
				{ "user_id", session.UserId },
				{ "refresh_sha256", session.RefreshDigest },
				{ "previous_sha256", session.PreviousDigest },
				{ "issued", session.IssuedUnix },
				{ "expiry", session.ExpiryUnix },
				{ "last_seen", session.LastSeenUnix },
				{ "user_agent", session.UserAgent },
				{ "peer_ip", session.PeerIp }
			};
		}

		SessionRecord FromJson(const nlohmann::json& json)
		{
			SessionRecord session;
			session.Id = json.value("id", "");
			session.UserId = json.value("user_id", "");
			session.RefreshDigest = json.value("refresh_sha256", "");
			session.PreviousDigest = json.value("previous_sha256", "");
			session.IssuedUnix = json.value("issued", std::int64_t{ 0 });
			session.ExpiryUnix = json.value("expiry", std::int64_t{ 0 });
			session.LastSeenUnix = json.value("last_seen", std::int64_t{ 0 });
			session.UserAgent = json.value("user_agent", "");
			session.PeerIp = json.value("peer_ip", "");
			return session;
		}
	}
	// anonymous namespace

	SessionStore SessionStore::Load(const std::filesystem::path& file)
	{
		SessionStore store;
		store.m_File = file;

		if (const auto document = LoadAuthStoreFile(file); document.has_value())
		{
			for (const auto& session_json : document->value("sessions", nlohmann::json::array()))
			{
				store.m_Sessions.push_back(FromJson(session_json));
			}
		}

		return store;
	}

	std::vector<SessionRecord> SessionStore::ForUser(std::string_view user_id) const
	{
		std::vector<SessionRecord> sessions;

		std::ranges::copy_if(m_Sessions, std::back_inserter(sessions), [&](const auto& session) { return session.UserId == user_id; });

		return sessions;
	}

	std::string SessionStore::Create(std::string user_id, std::int64_t expiry_unix, std::string user_agent, std::string peer_ip, std::int64_t now_unix, std::string& out_session_id)
	{
		const auto secret = GenerateSecret();

		SessionRecord session;
		session.Id = boost::uuids::to_string(boost::uuids::random_generator()());
		session.UserId = std::move(user_id);
		session.RefreshDigest = Sha256Hex(secret);
		session.IssuedUnix = now_unix;
		session.ExpiryUnix = expiry_unix;
		session.LastSeenUnix = now_unix;
		session.UserAgent = std::move(user_agent);
		session.PeerIp = std::move(peer_ip);

		out_session_id = session.Id;

		m_Sessions.push_back(std::move(session));
		Save();

		return secret;
	}

	SessionStore::RotateResult SessionStore::Rotate(std::string_view presented_secret, std::int64_t now_unix)
	{
		RotateResult result;

		if (presented_secret.empty())
		{
			return result;
		}

		const auto digest = Sha256Hex(presented_secret);

		// A rotated-out secret being replayed is the stolen-token signature:
		// revoke the whole session and surface the incident.
		if (const auto reused = std::ranges::find_if(m_Sessions, [&](const auto& session) { return session.PreviousDigest == digest; }); reused != m_Sessions.end())
		{
			result.ReuseDetected = true;
			result.SessionId = reused->Id;
			result.UserId = reused->UserId;

			m_Sessions.erase(reused);
			Save();

			return result;
		}

		const auto it = std::ranges::find_if(m_Sessions, [&](const auto& session) { return session.RefreshDigest == digest; });

		if (m_Sessions.end() == it)
		{
			return result;
		}

		if (now_unix >= it->ExpiryUnix)
		{
			m_Sessions.erase(it);
			Save();

			return result;
		}

		const auto new_secret = GenerateSecret();

		it->PreviousDigest = it->RefreshDigest;
		it->RefreshDigest = Sha256Hex(new_secret);
		it->LastSeenUnix = now_unix;

		result.Success = true;
		result.NewRefreshSecret = new_secret;
		result.SessionId = it->Id;
		result.UserId = it->UserId;

		Save();

		return result;
	}

	bool SessionStore::RevokeByRefresh(std::string_view presented_secret)
	{
		const auto digest = Sha256Hex(presented_secret);

		const auto removed = std::erase_if(m_Sessions, [&](const auto& session) { return session.RefreshDigest == digest; });

		if (removed > 0)
		{
			Save();
		}

		return removed > 0;
	}

	bool SessionStore::RevokeById(std::string_view session_id)
	{
		const auto removed = std::erase_if(m_Sessions, [&](const auto& session) { return session.Id == session_id; });

		if (removed > 0)
		{
			Save();
		}

		return removed > 0;
	}

	std::size_t SessionStore::RevokeAllForUser(std::string_view user_id)
	{
		const auto removed = std::erase_if(m_Sessions, [&](const auto& session) { return session.UserId == user_id; });

		if (removed > 0)
		{
			Save();
		}

		return removed;
	}

	void SessionStore::PruneExpired(std::int64_t now_unix)
	{
		const auto removed = std::erase_if(m_Sessions, [&](const auto& session) { return now_unix >= session.ExpiryUnix; });

		if (removed > 0)
		{
			Save();
		}
	}

	void SessionStore::Save() const
	{
		nlohmann::json document;
		auto sessions = nlohmann::json::array();

		for (const auto& session : m_Sessions)
		{
			sessions.push_back(ToJson(session));
		}

		document["sessions"] = std::move(sessions);

		SaveAuthStoreFile(m_File, std::move(document));
	}

	std::string SessionStore::GenerateSecret()
	{
		static constexpr char HEX_DIGITS[] = "0123456789abcdef";

		std::uint8_t random_bytes[REFRESH_RANDOM_BYTES];

		if (1 != RAND_bytes(random_bytes, static_cast<int>(sizeof(random_bytes))))
		{
			throw std::runtime_error("OpenSSL RAND_bytes failed while generating a refresh token");
		}

		std::string secret{ REFRESH_PREFIX };
		secret.reserve(secret.size() + (sizeof(random_bytes) * 2));

		for (const auto byte : random_bytes)
		{
			secret.push_back(HEX_DIGITS[byte >> 4]);
			secret.push_back(HEX_DIGITS[byte & 0x0F]);
		}

		return secret;
	}

}
// namespace AqualinkAutomate::Auth
