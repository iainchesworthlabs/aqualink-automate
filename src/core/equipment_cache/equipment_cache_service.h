#pragma once

#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include "options/options_equipment_options.h"

namespace AqualinkAutomate::Kernel { class DataHub; class HubLocator; }

namespace AqualinkAutomate::EquipmentCache
{

	//=========================================================================
	// EquipmentCacheService — persist the discovered equipment configuration
	// (pool config, system board, and the device list with stable UUIDs) so the
	// dashboard can show last-known equipment immediately on restart, before live
	// discovery completes.
	//
	// Load() pre-populates the DataHub at boot. Live discovery then finds those
	// devices BY LABEL and updates them in place (never duplicating), and
	// overwrites the pool config / board as it re-detects them. Transient state
	// (on/off) is intentionally NOT cached — it loads as Unknown and is corrected
	// by live data, so a slightly-changed installation self-heals.
	//
	// Saving is change-driven: a slow timer re-snapshots only when the config
	// fingerprint (labels, types, pool config, board) actually changes, plus a
	// final save on Stop(). Empty --equipment-cache-file disables everything.
	//=========================================================================
	class EquipmentCacheService
	{
	public:
		EquipmentCacheService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::Equipment::EquipmentSettings& settings);

		void Load();   // boot: pre-populate the DataHub from the cache file
		void Start();  // begin the change-detected periodic save
		void Stop();   // cancel the timer and write a final snapshot

		// Exposed for tests (and reused internally).
		nlohmann::json Snapshot() const;
		void ApplySnapshot(const nlohmann::json& json) const;
		void SaveNow() const;

	private:
		void ScheduleSave();
		std::string Fingerprint() const;

	private:
		Options::Equipment::EquipmentSettings m_Settings;
		std::shared_ptr<Kernel::DataHub> m_DataHub;
		boost::asio::steady_timer m_Timer;
		std::string m_LastFingerprint;
		bool m_Stopped{ false };
	};

}
// namespace AqualinkAutomate::EquipmentCache
