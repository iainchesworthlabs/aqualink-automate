#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/signals2.hpp>

#include "options/options_history_options.h"

namespace AqualinkAutomate::Kernel
{
	class DataHub;
	class HubLocator;
	class PreferencesHub;
	class DataHub_ConfigEvent;
}

namespace AqualinkAutomate::History
{

	class SqliteDb;

	//=========================================================================
	// HistoryService — an embedded SQLite time-series recorder.
	//
	// Subscribes to the DataHub config events and samples temperatures,
	// chemistry (salt/pH/ORP) and SWG %, plus device button-state transitions.
	// Sampling policy: change-driven with a per-series minimum interval to bound
	// the row rate, PLUS a periodic heartbeat so charts have points during steady
	// state.  Numeric series store value rows; state series store transitions
	// only.  Samples buffer in memory and flush in one transaction on a timer so
	// the single-threaded main loop never blocks on fsync (WAL + NORMAL).
	//
	// Disabled (empty --history-db) => the service is simply never constructed
	// and the read API returns 503.
	//=========================================================================
	class HistoryService
	{
	public:
		using ClockFn = std::function<std::int64_t()>;   // unix seconds (UTC)

		HistoryService(boost::asio::io_context& io_context, Kernel::HubLocator& hub_locator, const Options::History::HistorySettings& settings);
		~HistoryService();

		HistoryService(const HistoryService&) = delete;
		HistoryService& operator=(const HistoryService&) = delete;

		// Open the database, create the schema, subscribe to hub events and start
		// the flush / heartbeat / retention timers. Throws on a fatal DB error.
		void Start();
		void Stop();

		struct SeriesInfo
		{
			std::string key;
			std::string unit;
			std::string label;   // friendly display name (device series); empty for analog
			std::int64_t first_ts{ 0 };
			std::int64_t last_ts{ 0 };
			std::int64_t count{ 0 };
		};

		struct Point
		{
			std::int64_t ts{ 0 };
			double value{ 0.0 };
		};

		// Read API (used by WebRoute_History). Each flushes pending samples first.
		std::vector<SeriesInfo> ListSeries();
		bool SeriesExists(const std::string& key);
		// Bucket-averaged downsample of [from, to] into at most max_points buckets.
		std::vector<Point> QuerySeries(const std::string& key, std::int64_t from, std::int64_t to, int max_points);

		// Exposed for tests / timers.
		void Flush();
		void PurgeOld();
		void Heartbeat();

		// Record a numeric sample (throttled unless is_heartbeat). Public for tests.
		void RecordNumeric(const std::string& key, const std::string& unit, double value, bool is_heartbeat = false);
		// Record a device-state transition (never throttled, no heartbeat).
		void RecordState(const std::string& key, double value);
		// Record a device-state transition keyed by the button's stable identity,
		// carrying the (mutable) friendly label as series metadata. The canonical
		// key is the button UUID so a device that gets relabelled mid-session
		// (e.g. "Aux5" -> "Pool Light") never produces two series. Public for tests.
		void RecordDeviceState(const std::string& key, const std::string& label, double value);

		void SetClock(ClockFn clock) { m_Clock = std::move(clock); }

	private:
		void OnConfigEvent(const std::shared_ptr<Kernel::DataHub_ConfigEvent>& event);
		void SampleCurrentState(bool is_heartbeat);
		std::int64_t EnsureSeries(const std::string& key, const std::string& unit, const std::string& label = {});
		// Add the `label` column to pre-existing databases (idempotent).
		void MigrateSchema();
		// Fold a legacy label-keyed device series into the UUID-keyed series, then
		// drop the legacy row. No-op when the legacy series does not exist.
		void MergeLegacyDeviceSeries(const std::string& legacy_key, std::int64_t target_id);
		void ScheduleFlush();
		void ScheduleHeartbeat();
		void SchedulePurge();

	private:
		boost::asio::io_context& m_IoContext;
		Options::History::HistorySettings m_Settings;
		std::shared_ptr<Kernel::DataHub> m_DataHub;
		std::shared_ptr<Kernel::PreferencesHub> m_PreferencesHub;

		std::unique_ptr<SqliteDb> m_Db;
		ClockFn m_Clock;
		bool m_Running{ false };

		// key -> series row id (cache so we insert each series once).
		std::map<std::string, std::int64_t> m_SeriesIds;

		// key -> last persisted label, so a relabel updates the row only on change.
		std::map<std::string, std::string> m_SeriesLabels;

		// Legacy device-series keys whose merge has already been attempted, so the
		// one-time fold runs at most once per (legacy-key) per process.
		std::set<std::string> m_DeviceMergeChecked;

		// Throttle bookkeeping: key -> last buffered sample timestamp.
		std::map<std::string, std::int64_t> m_LastSampleTs;

		struct Buffered { std::int64_t series_id; std::int64_t ts; double value; };
		std::vector<Buffered> m_Buffer;

		boost::asio::steady_timer m_FlushTimer;
		boost::asio::steady_timer m_HeartbeatTimer;
		boost::asio::steady_timer m_PurgeTimer;

		boost::signals2::scoped_connection m_ConfigConn;

		// Minimum seconds between change-driven samples of the same series.
		static constexpr std::int64_t MIN_SAMPLE_INTERVAL_SECONDS = 60;
		// Heartbeat cadence.
		static constexpr int HEARTBEAT_SECONDS = 300;
	};

}
// namespace AqualinkAutomate::History
