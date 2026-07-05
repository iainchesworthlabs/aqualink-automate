#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>

#include <boost/uuid/uuid_io.hpp>

#include "history/history_service.h"
#include "history/sqlite_db.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/hub_events/data_hub_config_event.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "kernel/hub_events/data_hub_config_event_chemistry.h"
#include "kernel/hub_events/data_hub_config_event_temperature.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "logging/logging.h"
#include "utility/slugify.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::History
{
	namespace
	{
		std::int64_t SystemClockSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		// A device button is "active" (1.0) when its status reads on/running/etc.
		double StateToValue(std::string_view status)
		{
			std::string lower;
			lower.reserve(status.size());
			for (const char c : status) { lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); }
			return (lower == "on" || lower == "running" || lower == "enabled" || lower == "heating") ? 1.0 : 0.0;
		}
	}
	// unnamed namespace

	HistoryService::HistoryService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::History::HistorySettings& settings) :
		m_Settings(settings),
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_PreferencesHub(hub_locator.Find<Kernel::PreferencesHub>()),
		m_Clock(SystemClockSeconds),
		m_FlushTimer(io_context),
		m_HeartbeatTimer(io_context),
		m_PurgeTimer(io_context)
	{
	}

	HistoryService::~HistoryService()
	{
		Stop();
	}

	void HistoryService::Start()
	{
		if (m_Running || m_Settings.db_path.empty())
		{
			return;
		}

		m_Db = std::make_unique<SqliteDb>(m_Settings.db_path);

		// WAL + NORMAL keeps writes off the fsync-per-commit path so the buffered
		// flush below never stalls the main loop.
		m_Db->Exec("PRAGMA journal_mode=WAL;");
		m_Db->Exec("PRAGMA synchronous=NORMAL;");
		m_Db->Exec(
			"CREATE TABLE IF NOT EXISTS series ("
			"  id INTEGER PRIMARY KEY,"
			"  key TEXT UNIQUE NOT NULL,"
			"  unit TEXT,"
			"  label TEXT);"
			"CREATE TABLE IF NOT EXISTS samples ("
			"  series_id INTEGER NOT NULL REFERENCES series(id),"
			"  ts INTEGER NOT NULL,"
			"  value REAL NOT NULL);"
			"CREATE INDEX IF NOT EXISTS idx_samples_series_ts ON samples(series_id, ts);");

		// Bring a pre-existing database (created before the `label` column) up to date.
		MigrateSchema();

		m_Running = true;

		if (m_DataHub)
		{
			m_ConfigConn = m_DataHub->ConfigUpdateSignal.connect(
				[this](const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event) { OnConfigEvent(event); });
		}

		ScheduleFlush();
		ScheduleHeartbeat();
		SchedulePurge();

		LogInfo(Channel::Main, [this] { return std::format("History service started (db='{}', retention={}d)", m_Settings.db_path, m_Settings.retention_days); });
	}

	void HistoryService::Stop()
	{
		if (!m_Running)
		{
			return;
		}
		m_Running = false;

		m_ConfigConn.disconnect();
		m_FlushTimer.cancel();
		m_HeartbeatTimer.cancel();
		m_PurgeTimer.cancel();

		// Flush whatever is buffered so a clean shutdown loses nothing.
		try { Flush(); } catch (const std::exception& ex) { LogWarning(Channel::Main, [&ex] { return std::format("History flush on shutdown failed: {}", ex.what()); }); }

		m_Db.reset();
	}

	void HistoryService::OnConfigEvent(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event)
	{
		if (!event)
		{
			return;
		}

		try
		{
			if (auto temp = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_Temperature>(event))
			{
				if (auto v = temp->PoolTemp(); v.has_value()) { RecordNumeric("temp/pool", "C", v->InCelsius().value()); }
				if (auto v = temp->SpaTemp(); v.has_value()) { RecordNumeric("temp/spa", "C", v->InCelsius().value()); }
				if (auto v = temp->AirTemp(); v.has_value()) { RecordNumeric("temp/air", "C", v->InCelsius().value()); }
			}
			else if (auto chem = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_Chemistry>(event))
			{
				if (auto v = chem->SaltLevel(); v.has_value() && v->value() > 0.0) { RecordNumeric("chem/salt_ppm", "ppm", v->value()); }
				if (auto v = chem->pH(); v.has_value() && static_cast<double>((*v)()) > 0.0) { RecordNumeric("chem/ph", "pH", static_cast<double>((*v)())); }
				if (auto v = chem->ORP(); v.has_value() && (*v)().value() > 0.0) { RecordNumeric("chem/orp", "mV", (*v)().value()); }
			}
			else if (auto button = std::dynamic_pointer_cast<Kernel::DataHub_ConfigEvent_ButtonStateChange>(event))
			{
				// Key on the button's stable UUID, not its (mutable) label: a device
				// that boots as "Aux5" and is renamed "Pool Light" once discovered
				// keeps a single series instead of spawning a second one.
				const std::string key = std::format("device/{}", boost::uuids::to_string(button->ButtonId()));
				RecordDeviceState(key, std::string{ button->Label() }, StateToValue(button->Status()));
			}
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&ex] { return std::format("History record from config event failed: {}", ex.what()); });
		}
	}

	void HistoryService::SampleCurrentState(bool is_heartbeat)
	{
		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		if (auto v = m_DataHub->PoolTemp(); v.has_value()) { RecordNumeric("temp/pool", "C", v->InCelsius().value(), is_heartbeat); }
		if (auto v = m_DataHub->SpaTemp(); v.has_value()) { RecordNumeric("temp/spa", "C", v->InCelsius().value(), is_heartbeat); }
		if (auto v = m_DataHub->AirTemp(); v.has_value()) { RecordNumeric("temp/air", "C", v->InCelsius().value(), is_heartbeat); }

		if (const double salt = m_DataHub->SaltLevel().value(); salt > 0.0) { RecordNumeric("chem/salt_ppm", "ppm", salt, is_heartbeat); }
		if (const double orp = m_DataHub->ORP()().value(); orp > 0.0) { RecordNumeric("chem/orp", "mV", orp, is_heartbeat); }
		if (const auto ph = static_cast<double>(m_DataHub->pH()()); ph > 0.0) { RecordNumeric("chem/ph", "pH", ph, is_heartbeat); }

		if (auto chlorinators = m_DataHub->Chlorinators(); !chlorinators.empty())
		{
			if (auto duty = chlorinators.front()->AuxillaryTraits.TryGet(DutyCycleTrait{}); duty.has_value())
			{
				RecordNumeric("swg/percent", "%", static_cast<double>(duty.value()), is_heartbeat);
			}
		}
	}

	std::int64_t HistoryService::EnsureSeries(const std::string& key, const std::string& unit, const std::string& label)
	{
		if (const auto cached = m_SeriesIds.find(key); cached != m_SeriesIds.end())
		{
			// Known series — update the friendly label in place if it changed
			// (a device discovered after boot, e.g. "Aux5" -> "Pool Light").
			if (!label.empty() && m_SeriesLabels[key] != label)
			{
				SqliteStmt update(*m_Db, "UPDATE series SET label = ? WHERE key = ?");
				update.Bind(1, label);
				update.Bind(2, key);
				update.Step();
				m_SeriesLabels[key] = label;
			}
			return cached->second;
		}

		{
			SqliteStmt insert(*m_Db, "INSERT OR IGNORE INTO series(key, unit, label) VALUES(?, ?, ?)");
			insert.Bind(1, key);
			insert.Bind(2, unit);
			insert.Bind(3, label);
			insert.Step();
		}

		std::int64_t id = 0;
		{
			SqliteStmt select(*m_Db, "SELECT id FROM series WHERE key = ?");
			select.Bind(1, key);
			if (select.Step())
			{
				id = select.ColumnInt64(0);
			}
		}

		m_SeriesIds[key] = id;
		if (!label.empty()) { m_SeriesLabels[key] = label; }
		return id;
	}

	void HistoryService::MigrateSchema()
	{
		if (!m_Db)
		{
			return;
		}

		// Add the `label` column to databases created before it existed. SQLite
		// has no "ADD COLUMN IF NOT EXISTS", so probe the table and ALTER only when
		// the column is absent.
		try
		{
			bool has_label = false;
			{
				SqliteStmt info(*m_Db, "PRAGMA table_info(series)");
				while (info.Step())
				{
					if (info.ColumnText(1) == "label") { has_label = true; break; }
				}
			}
			if (!has_label)
			{
				m_Db->Exec("ALTER TABLE series ADD COLUMN label TEXT");
			}
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&ex] { return std::format("History schema migration failed: {}", ex.what()); });
		}
	}

	void HistoryService::MergeLegacyDeviceSeries(const std::string& legacy_key, std::int64_t target_id)
	{
		// Find the legacy label-keyed series, if any.
		std::int64_t legacy_id = 0;
		{
			SqliteStmt select(*m_Db, "SELECT id FROM series WHERE key = ?");
			select.Bind(1, legacy_key);
			if (!select.Step())
			{
				return;   // no legacy series for this label — nothing to fold
			}
			legacy_id = select.ColumnInt64(0);
		}

		if (legacy_id == target_id)
		{
			return;
		}

		// Re-point the legacy samples onto the canonical (UUID) series, then drop
		// the now-empty legacy series row, in one transaction.
		SqliteTransaction txn(*m_Db);
		{
			SqliteStmt repoint(*m_Db, "UPDATE samples SET series_id = ? WHERE series_id = ?");
			repoint.Bind(1, target_id);
			repoint.Bind(2, legacy_id);
			repoint.Step();
		}
		{
			SqliteStmt drop(*m_Db, "DELETE FROM series WHERE id = ?");
			drop.Bind(1, legacy_id);
			drop.Step();
		}
		txn.Commit();

		// Forget any cached state for the now-deleted legacy key.
		m_SeriesIds.erase(legacy_key);
		m_SeriesLabels.erase(legacy_key);
		m_LastSampleTs.erase(legacy_key);

		LogInfo(Channel::Main, [&legacy_key] { return std::format("History merged legacy device series '{}' into the UUID-keyed series", legacy_key); });
	}

	void HistoryService::RecordNumeric(const std::string& key, const std::string& unit, double value, bool is_heartbeat)
	{
		if (!m_Running || !m_Db)
		{
			return;
		}

		const std::int64_t now = m_Clock();

		// Change-driven samples are throttled to bound the row rate; the periodic
		// heartbeat bypasses the throttle so steady state still produces points.
		if (!is_heartbeat)
		{
			if (auto it = m_LastSampleTs.find(key); it != m_LastSampleTs.end() && (now - it->second) < MIN_SAMPLE_INTERVAL_SECONDS)
			{
				return;
			}
		}

		try
		{
			const std::int64_t series_id = EnsureSeries(key, unit);
			m_Buffer.emplace_back(series_id, now, value);
			m_LastSampleTs[key] = now;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&key, &ex] { return std::format("History RecordNumeric('{}') failed: {}", key, ex.what()); });
		}
	}

	void HistoryService::RecordState(const std::string& key, double value)
	{
		if (!m_Running || !m_Db)
		{
			return;
		}

		try
		{
			const std::int64_t series_id = EnsureSeries(key, "state");
			m_Buffer.emplace_back(series_id, m_Clock(), value);
			m_LastSampleTs[key] = m_Clock();
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&key, &ex] { return std::format("History RecordState('{}') failed: {}", key, ex.what()); });
		}
	}

	void HistoryService::RecordDeviceState(const std::string& key, const std::string& label, double value)
	{
		if (!m_Running || !m_Db)
		{
			return;
		}

		try
		{
			const std::int64_t series_id = EnsureSeries(key, "state", label);

			// One-time fold of any legacy label-keyed series (`device/<slug>/state`,
			// the pre-UUID scheme) into this canonical series. Runs at most once per
			// observed label so the cost is a single lookup per device per process.
			if (!label.empty())
			{
				const std::string legacy_key = std::format("device/{}/state", Utility::Slugify(label));
				if (legacy_key != key && m_DeviceMergeChecked.insert(legacy_key).second)
				{
					MergeLegacyDeviceSeries(legacy_key, series_id);
				}
			}

			m_Buffer.emplace_back(series_id, m_Clock(), value);
			m_LastSampleTs[key] = m_Clock();
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&key, &ex] { return std::format("History RecordDeviceState('{}') failed: {}", key, ex.what()); });
		}
	}

	void HistoryService::Flush()
	{
		if (!m_Db || m_Buffer.empty())
		{
			return;
		}

		try
		{
			SqliteTransaction txn(*m_Db);
			SqliteStmt insert(*m_Db, "INSERT INTO samples(series_id, ts, value) VALUES(?, ?, ?)");
			for (const auto& sample : m_Buffer)
			{
				insert.Reset();
				insert.Bind(1, sample.series_id);
				insert.Bind(2, sample.ts);
				insert.Bind(3, sample.value);
				insert.Step();
			}
			txn.Commit();
			m_Buffer.clear();
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&ex] { return std::format("History flush failed (retaining buffer): {}", ex.what()); });
		}
	}

	void HistoryService::PurgeOld()
	{
		// Read retention LIVE from preferences (seeded from the CLI at boot).
		const std::uint32_t retention_days = m_PreferencesHub ? m_PreferencesHub->HistoryRetentionDays : m_Settings.retention_days;

		if (!m_Db || retention_days == 0)
		{
			return;
		}

		try
		{
			const std::int64_t cutoff = m_Clock() - (static_cast<std::int64_t>(retention_days) * 86400);
			SqliteStmt del(*m_Db, "DELETE FROM samples WHERE ts < ?");
			del.Bind(1, cutoff);
			del.Step();
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Main, [&ex] { return std::format("History retention purge failed: {}", ex.what()); });
		}
	}

	void HistoryService::Heartbeat()
	{
		SampleCurrentState(/*is_heartbeat=*/true);
	}

	std::vector<HistoryService::SeriesInfo> HistoryService::ListSeries()
	{
		std::vector<SeriesInfo> result;
		if (!m_Db)
		{
			return result;
		}

		Flush();

		SqliteStmt stmt(*m_Db,
			"SELECT s.key, s.unit, s.label, MIN(sm.ts), MAX(sm.ts), COUNT(*) "
			"FROM series s JOIN samples sm ON sm.series_id = s.id "
			"GROUP BY s.id ORDER BY s.key");
		while (stmt.Step())
		{
			SeriesInfo info;
			info.key = stmt.ColumnText(0);
			info.unit = stmt.ColumnText(1);
			info.label = stmt.ColumnText(2);   // empty for analog series / NULL rows
			info.first_ts = stmt.ColumnInt64(3);
			info.last_ts = stmt.ColumnInt64(4);
			info.count = stmt.ColumnInt64(5);
			result.push_back(std::move(info));
		}
		return result;
	}

	bool HistoryService::SeriesExists(const std::string& key)
	{
		if (!m_Db)
		{
			return false;
		}
		SqliteStmt stmt(*m_Db, "SELECT 1 FROM series WHERE key = ? LIMIT 1");
		stmt.Bind(1, key);
		return stmt.Step();
	}

	std::vector<HistoryService::Point> HistoryService::QuerySeries(const std::string& key, std::int64_t from, std::int64_t to, int max_points)
	{
		std::vector<Point> result;
		if (!m_Db || max_points <= 0 || to < from)
		{
			return result;
		}

		Flush();

		// Bucket width (seconds) so the result has at most ~max_points buckets.
		const std::int64_t span = (to - from);
		std::int64_t bucket = span / max_points;
		if (bucket < 1) { bucket = 1; }

		// Time-bucket mean: floor each ts to its bucket and average the values.
		SqliteStmt stmt(*m_Db,
			"SELECT (sm.ts / ?) * ? AS bucket, AVG(sm.value) "
			"FROM samples sm JOIN series s ON s.id = sm.series_id "
			"WHERE s.key = ? AND sm.ts >= ? AND sm.ts <= ? "
			"GROUP BY bucket ORDER BY bucket");
		stmt.Bind(1, bucket);
		stmt.Bind(2, bucket);
		stmt.Bind(3, key);
		stmt.Bind(4, from);
		stmt.Bind(5, to);

		while (stmt.Step())
		{
			result.push_back(Point{ stmt.ColumnInt64(0), stmt.ColumnDouble(1) });
		}
		return result;
	}

	void HistoryService::ScheduleFlush()
	{
		if (!m_Running) { return; }
		m_FlushTimer.expires_after(std::chrono::seconds(std::max<std::uint32_t>(1, m_Settings.flush_seconds)));
		m_FlushTimer.async_wait([this](const boost::system::error_code& ec)
		{
			if (ec || !m_Running) { return; }
			Flush();
			ScheduleFlush();
		});
	}

	void HistoryService::ScheduleHeartbeat()
	{
		if (!m_Running) { return; }
		m_HeartbeatTimer.expires_after(std::chrono::seconds(HEARTBEAT_SECONDS));
		m_HeartbeatTimer.async_wait([this](const boost::system::error_code& ec)
		{
			if (ec || !m_Running) { return; }
			Heartbeat();
			ScheduleHeartbeat();
		});
	}

	void HistoryService::SchedulePurge()
	{
		if (!m_Running) { return; }
		// Daily.
		m_PurgeTimer.expires_after(std::chrono::hours(24));
		m_PurgeTimer.async_wait([this](const boost::system::error_code& ec)
		{
			if (ec || !m_Running) { return; }
			PurgeOld();
			SchedulePurge();
		});
	}

}
// namespace AqualinkAutomate::History
