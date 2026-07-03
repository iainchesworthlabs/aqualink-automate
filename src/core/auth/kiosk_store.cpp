#include <utility>

#include <nlohmann/json.hpp>

#include "auth/auth_store_file.h"
#include "auth/kiosk_store.h"

namespace AqualinkAutomate::Auth
{

	KioskStore KioskStore::Load(const std::filesystem::path& file)
	{
		KioskStore store;
		store.m_File = file;

		if (const auto document = LoadAuthStoreFile(file); document.has_value())
		{
			store.m_Enabled = document->value("enabled", false);
			store.m_PinHash = document->value("pin_hash", "");
			store.m_TargetGroup = document->value("target_group", "");
			store.m_TokenVersion = document->value("token_version", std::uint32_t{ 1 });
		}

		return store;
	}

	void KioskStore::Configure(std::string pin_hash, std::string target_group)
	{
		m_Enabled = true;
		m_PinHash = std::move(pin_hash);
		m_TargetGroup = std::move(target_group);
		++m_TokenVersion;   // Any prior kiosk access token is now stale.
		Save();
	}

	void KioskStore::Disable()
	{
		m_Enabled = false;
		m_PinHash.clear();
		m_TargetGroup.clear();
		++m_TokenVersion;
		Save();
	}

	void KioskStore::Save() const
	{
		nlohmann::json document;
		document["enabled"] = m_Enabled;
		document["pin_hash"] = m_PinHash;
		document["target_group"] = m_TargetGroup;
		document["token_version"] = m_TokenVersion;

		SaveAuthStoreFile(m_File, std::move(document));
	}

}
// namespace AqualinkAutomate::Auth
