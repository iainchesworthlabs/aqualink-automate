#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>

#include "exceptions/exception_history_databaseerror.h"
#include "history/history_service.h"
#include "history/sqlite_db.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/orp.h"
#include "kernel/ph.h"
#include "kernel/preferences_hub.h"
#include "kernel/temperature.h"
#include "options/options_history_options.h"
#include "types/units_dimensionless.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

namespace
{
	Options::History::HistorySettings MemorySettings()
	{
		Options::History::HistorySettings s;
		s.db_path = ":memory:";
		s.retention_days = 90;
		s.flush_seconds = 10;
		return s;
	}
}

//=============================================================================
// RAII SQLite wrapper
//=============================================================================
BOOST_AUTO_TEST_SUITE(TestSuite_SqliteDb)

BOOST_AUTO_TEST_CASE(SqliteDb_PreparedStatementRoundTrip)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, k TEXT, v REAL);");

	{
		History::SqliteStmt insert(db, "INSERT INTO t(k, v) VALUES(?, ?)");
		insert.Bind(1, std::string{ "alpha" });
		insert.Bind(2, 3.5);
		BOOST_CHECK(!insert.Step()); // INSERT yields no row
	}

	History::SqliteStmt select(db, "SELECT k, v FROM t WHERE k = ?");
	select.Bind(1, std::string{ "alpha" });
	BOOST_REQUIRE(select.Step());
	BOOST_CHECK_EQUAL(select.ColumnText(0), "alpha");
	BOOST_CHECK_EQUAL(select.ColumnDouble(1), 3.5);
	BOOST_CHECK(!select.Step());
}

BOOST_AUTO_TEST_CASE(SqliteDb_TransactionRollbackOnScopeExit)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(v INTEGER);");

	{
		History::SqliteTransaction txn(db);
		db.Exec("INSERT INTO t(v) VALUES(1);");
		// No Commit() -> destructor rolls back.
	}

	History::SqliteStmt count(db, "SELECT COUNT(*) FROM t");
	BOOST_REQUIRE(count.Step());
	BOOST_CHECK_EQUAL(count.ColumnInt64(0), 0);
}

BOOST_AUTO_TEST_CASE(SqliteDb_OpenInvalidPathThrows)
{
	// A path under a non-existent directory cannot be created.
	BOOST_CHECK_THROW(History::SqliteDb db("R:/this/dir/does/not/exist/h.db"), AqualinkAutomate::Exceptions::History_DatabaseError);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HistoryService sampling / downsampling / retention
//=============================================================================
BOOST_FIXTURE_TEST_SUITE(TestSuite_HistoryService, Test::HubLocatorInjector)

BOOST_AUTO_TEST_CASE(RecordNumeric_ThrottlesWithinMinInterval)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 1'000'000;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordNumeric("temp/pool", "C", 20.0);           // buffered
	now += 30;                                                // within 60s
	service.RecordNumeric("temp/pool", "C", 21.0);           // throttled
	now += 40;                                                // now 70s after first
	service.RecordNumeric("temp/pool", "C", 22.0);           // buffered

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "temp/pool");
	BOOST_CHECK_EQUAL(series.front().unit, "C");
	BOOST_CHECK_EQUAL(series.front().count, 2); // throttled middle sample dropped
}

BOOST_AUTO_TEST_CASE(RecordNumeric_HeartbeatBypassesThrottle)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 1'000'000;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordNumeric("temp/pool", "C", 20.0);                         // buffered
	now += 5;
	service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true);  // bypass -> buffered

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().count, 2);
}

BOOST_AUTO_TEST_CASE(QuerySeries_BucketAveragedDownsample)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 0;
	service.SetClock([&now] { return now; });
	service.Start();

	// Four samples at ts 0,10,20,30 -> values 2,4,6,8 (heartbeat bypasses throttle).
	const std::int64_t ts[] = { 0, 10, 20, 30 };
	const double vals[] = { 2.0, 4.0, 6.0, 8.0 };
	for (int i = 0; i < 4; ++i)
	{
		now = ts[i];
		service.RecordNumeric("temp/pool", "C", vals[i], /*is_heartbeat=*/true);
	}

	// from=0,to=40,max_points=2 -> bucket=20. floor(ts/20)*20: {0,10}->0, {20,30}->20.
	auto points = service.QuerySeries("temp/pool", 0, 40, 2);
	BOOST_REQUIRE_EQUAL(points.size(), 2u);
	BOOST_CHECK_EQUAL(points[0].ts, 0);
	BOOST_CHECK_EQUAL(points[0].value, 3.0); // avg(2,4)
	BOOST_CHECK_EQUAL(points[1].ts, 20);
	BOOST_CHECK_EQUAL(points[1].value, 7.0); // avg(6,8)
}

BOOST_AUTO_TEST_CASE(QuerySeries_UnknownKeyEmpty_KnownKeyExists)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 500;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordNumeric("chem/salt_ppm", "ppm", 3200.0, true);
	service.Flush();

	BOOST_CHECK(service.SeriesExists("chem/salt_ppm"));
	BOOST_CHECK(!service.SeriesExists("nope/missing"));
}

BOOST_AUTO_TEST_CASE(PurgeOld_DropsSamplesBeyondRetention)
{
	boost::asio::io_context io;
	auto settings = MemorySettings();
	settings.retention_days = 90;
	History::HistoryService service(io, *this, settings);

	std::int64_t now = 0;
	service.SetClock([&now] { return now; });
	service.Start();

	// PurgeOld reads retention live from PreferencesHub (seeded from the CLI).
	Find<Kernel::PreferencesHub>()->HistoryRetentionDays = 90;

	// One sample 100 days ago, one "now".
	const std::int64_t reference = 1'000'000'000;
	now = reference - (100 * 86400);
	service.RecordNumeric("temp/pool", "C", 10.0, true);
	now = reference;
	service.RecordNumeric("temp/pool", "C", 20.0, true);
	service.Flush();

	service.PurgeOld(); // cutoff = now - 90d -> the 100-day-old row is removed.

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().count, 1);
}

BOOST_AUTO_TEST_CASE(RecordState_StoresTransitionsAsStateSeries)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 42;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordState("device/pool_pump/state", 1.0);
	now += 5;
	service.RecordState("device/pool_pump/state", 0.0); // transitions are never throttled

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/pool_pump/state");
	BOOST_CHECK_EQUAL(series.front().unit, "state");
	BOOST_CHECK_EQUAL(series.front().count, 2);
}

BOOST_AUTO_TEST_CASE(RecordDeviceState_KeysByUuidAndCarriesLabel)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 100;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordDeviceState("device/uuid-aux5", "Pool Light", 1.0);

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/uuid-aux5");
	BOOST_CHECK_EQUAL(series.front().unit, "state");
	BOOST_CHECK_EQUAL(series.front().label, "Pool Light");
	BOOST_CHECK_EQUAL(series.front().count, 1);
}

BOOST_AUTO_TEST_CASE(RecordDeviceState_RelabelUpdatesInPlaceWithoutDuplicating)
{
	// A device boots as "Aux5" then is renamed "Pool Light" once discovered. The
	// UUID key is stable across the rename, so a single series accumulates and the
	// most-recent label wins — this is the duplicate-in-the-filter fix.
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 100;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordDeviceState("device/uuid-1", "Aux5", 1.0);
	now += 5;
	service.RecordDeviceState("device/uuid-1", "Pool Light", 0.0);

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/uuid-1");
	BOOST_CHECK_EQUAL(series.front().label, "Pool Light");
	BOOST_CHECK_EQUAL(series.front().count, 2);
}

BOOST_AUTO_TEST_CASE(RecordDeviceState_FoldsLegacyLabelKeyedSeries)
{
	// Simulate an old database holding a legacy label-keyed series. The first
	// UUID-keyed recording with the matching label folds the legacy samples into
	// the canonical series and drops the legacy row, leaving exactly one series.
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 100;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordState("device/pool_light/state", 1.0);   // legacy scheme
	now += 5;
	service.RecordState("device/pool_light/state", 0.0);
	service.Flush();

	now += 5;
	service.RecordDeviceState("device/uuid-1", "Pool Light", 1.0);   // canonical scheme
	service.Flush();

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/uuid-1");
	BOOST_CHECK_EQUAL(series.front().label, "Pool Light");
	BOOST_CHECK_EQUAL(series.front().count, 3);   // 2 legacy + 1 new, merged
}

//=============================================================================
// Lifecycle / maintenance guards (Flush, PurgeOld, Stop, Heartbeat, disabled).
//=============================================================================

BOOST_AUTO_TEST_CASE(Flush_EmptyBuffer_IsNoOp)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	service.Start();

	// Nothing buffered: Flush takes the empty-buffer early return and leaves the DB empty.
	BOOST_CHECK_NO_THROW(service.Flush());
	BOOST_CHECK(service.ListSeries().empty());
}

BOOST_AUTO_TEST_CASE(PurgeOld_RetentionZero_KeepsAllSamples)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 1'000'000;
	service.SetClock([&now] { return now; });
	service.Start();

	// Retention 0 disables purging entirely (keep-forever), so PurgeOld is a no-op.
	Find<Kernel::PreferencesHub>()->HistoryRetentionDays = 0;

	service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true);
	now += 5;
	service.RecordNumeric("temp/pool", "C", 21.0, /*is_heartbeat=*/true);
	service.Flush();

	service.PurgeOld();

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().count, 2);   // nothing purged
}

BOOST_AUTO_TEST_CASE(Stop_AfterStart_FlushesCleanly)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 10;
	service.SetClock([&now] { return now; });
	service.Start();

	service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true);

	// Stop flushes the outstanding buffer and tears down timers without throwing.
	BOOST_CHECK_NO_THROW(service.Stop());
}

BOOST_AUTO_TEST_CASE(Heartbeat_OnEmptyDataHub_RecordsNothing)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 1'000;
	service.SetClock([&now] { return now; });
	service.Start();

	// With no temps/chemistry/chlorinator on the DataHub, SampleCurrentState takes every
	// "absent value" branch and records nothing.
	BOOST_CHECK_NO_THROW(service.Heartbeat());
	BOOST_CHECK(service.ListSeries().empty());
}

BOOST_AUTO_TEST_CASE(Start_EmptyDbPath_StaysDisabled)
{
	boost::asio::io_context io;
	auto settings = MemorySettings();
	settings.db_path = "";   // disabled: no database is opened
	History::HistoryService service(io, *this, settings);

	BOOST_CHECK_NO_THROW(service.Start());

	// A disabled service silently drops samples and reports no series.
	BOOST_CHECK_NO_THROW(service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true));
	BOOST_CHECK(service.ListSeries().empty());
}

// The read API is safe to call on a never-Started service: with no open database
// SeriesExists / QuerySeries / ListSeries all take their `!m_Db` early returns.
BOOST_AUTO_TEST_CASE(ReadApi_BeforeStart_NoDb_ReturnsEmpty)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	// Never Started -> m_Db is null; every read takes the no-db guard.
	BOOST_CHECK(!service.SeriesExists("temp/pool"));
	BOOST_CHECK(service.ListSeries().empty());
	BOOST_CHECK(service.QuerySeries("temp/pool", 0, 100, 10).empty());
}

// QuerySeries rejects degenerate arguments before touching the database: a
// non-positive max_points and an inverted [from, to] window both return empty.
BOOST_AUTO_TEST_CASE(QuerySeries_InvalidArgs_ReturnEmpty)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 0;
	service.SetClock([&now] { return now; });
	service.Start();

	// A real series exists, so only the argument guards can be responsible for the
	// empty result.
	service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true);
	service.Flush();

	BOOST_CHECK(service.QuerySeries("temp/pool", 0, 100, 0).empty());   // max_points <= 0
	BOOST_CHECK(service.QuerySeries("temp/pool", 0, 100, -5).empty());  // max_points <= 0
	BOOST_CHECK(service.QuerySeries("temp/pool", 100, 0, 10).empty());  // to < from
}

// max_points wider than the sampled span forces the bucket floor to 1 (the
// `bucket < 1` clamp), so every distinct timestamp becomes its own point.
BOOST_AUTO_TEST_CASE(QuerySeries_MaxPointsExceedsSpan_ClampsBucketToOne)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 0;
	service.SetClock([&now] { return now; });
	service.Start();

	// Two samples one second apart; span=1, max_points=100 -> bucket=0 -> clamped to 1.
	now = 10;
	service.RecordNumeric("temp/pool", "C", 20.0, /*is_heartbeat=*/true);
	now = 11;
	service.RecordNumeric("temp/pool", "C", 21.0, /*is_heartbeat=*/true);

	auto points = service.QuerySeries("temp/pool", 10, 11, 100);
	BOOST_REQUIRE_EQUAL(points.size(), 2u);   // each ts is its own bucket
	BOOST_CHECK_EQUAL(points.front().value, 20.0);
	BOOST_CHECK_EQUAL(points.back().value, 21.0);
}

// A device that never carries a friendly label records via the empty-label branch
// of RecordDeviceState (no relabel, no legacy fold), storing a state series whose
// label column stays empty.
BOOST_AUTO_TEST_CASE(RecordDeviceState_EmptyLabel_NoLegacyFoldOrRelabel)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 100;
	service.SetClock([&now] { return now; });
	service.Start();

	// Empty label -> the `!label.empty()` guards around relabel + legacy-fold are all
	// skipped; a single unlabelled state series results.
	service.RecordDeviceState("device/uuid-nolabel", "", 1.0);

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/uuid-nolabel");
	BOOST_CHECK(series.front().label.empty());
	BOOST_CHECK_EQUAL(series.front().count, 1);
}

// The legacy-fold check runs at most once per legacy key per process: a second
// RecordDeviceState for the same device does not re-attempt the merge (the
// m_DeviceMergeChecked guard), and the series keeps accumulating.
BOOST_AUTO_TEST_CASE(RecordDeviceState_LegacyFoldCheckedOncePerKey)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 100;
	service.SetClock([&now] { return now; });
	service.Start();

	// No legacy series exists, so the first fold attempt is a no-op; the second
	// recording re-enters with the merge already marked checked.
	service.RecordDeviceState("device/uuid-1", "Pool Light", 1.0);
	now += 5;
	service.RecordDeviceState("device/uuid-1", "Pool Light", 0.0);

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().key, "device/uuid-1");
	BOOST_CHECK_EQUAL(series.front().count, 2);
}

//=============================================================================
// OnConfigEvent: after Start() the service subscribes to the DataHub's
// ConfigUpdateSignal and records temperature / chemistry / device-state events.
// Driving the real DataHub setters exercises the event-dispatch branches and the
// StateToValue helper without touching the private OnConfigEvent directly.
//=============================================================================

BOOST_AUTO_TEST_CASE(OnConfigEvent_TemperatureEvent_RecordsPoolSpaAir)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 5'000;
	service.SetClock([&now] { return now; });
	service.Start();

	auto data_hub = Find<Kernel::DataHub>();

	// Each distinct reading fans out a Temperature config event; OnConfigEvent
	// records the corresponding temp/<body> series (in Celsius).
	data_hub->PoolTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(28.0));
	now += 100;
	data_hub->SpaTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(36.0));
	now += 100;
	data_hub->AirTemp(Kernel::Temperature::ConvertToTemperatureInCelsius(19.0));

	auto series = service.ListSeries();
	bool saw_pool = false, saw_spa = false, saw_air = false;
	for (const auto& s : series)
	{
		if (s.key == "temp/pool") { saw_pool = true; }
		if (s.key == "temp/spa") { saw_spa = true; }
		if (s.key == "temp/air") { saw_air = true; }
	}
	BOOST_CHECK(saw_pool);
	BOOST_CHECK(saw_spa);
	BOOST_CHECK(saw_air);
}

BOOST_AUTO_TEST_CASE(OnConfigEvent_ChemistryEvent_RecordsPositiveValuesOnly)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 6'000;
	service.SetClock([&now] { return now; });
	service.Start();

	auto data_hub = Find<Kernel::DataHub>();

	// A positive salt reading fans out a Chemistry event and is recorded; each
	// chemistry setter emits its own single-field event.
	data_hub->SaltLevel(3200.0 * Units::ppm);
	now += 100;
	data_hub->pH(Kernel::pH(7.4f));
	now += 100;
	data_hub->ORP(Kernel::ORP(720.0));

	auto series = service.ListSeries();
	bool saw_salt = false, saw_ph = false, saw_orp = false;
	for (const auto& s : series)
	{
		if (s.key == "chem/salt_ppm") { saw_salt = true; }
		if (s.key == "chem/ph") { saw_ph = true; }
		if (s.key == "chem/orp") { saw_orp = true; }
	}
	BOOST_CHECK(saw_salt);
	BOOST_CHECK(saw_ph);
	BOOST_CHECK(saw_orp);
}

BOOST_AUTO_TEST_CASE(OnConfigEvent_ButtonStateChange_RecordsDeviceStateWithStateValue)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 7'000;
	service.SetClock([&now] { return now; });
	service.Start();

	auto data_hub = Find<Kernel::DataHub>();

	boost::uuids::string_generator gen;
	const auto button = gen("01234567-89ab-cdef-0123-456789abcdef");

	// "Running" maps (via StateToValue) to 1.0; a subsequent "Off" maps to 0.0.
	// The series is keyed on the button UUID and carries the friendly label.
	data_hub->EmitButtonStateChange(button, "Running", "Filter Pump");
	now += 100;
	data_hub->EmitButtonStateChange(button, "Off", "Filter Pump");

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().unit, "state");
	BOOST_CHECK_EQUAL(series.front().label, "Filter Pump");
	BOOST_CHECK_EQUAL(series.front().count, 2);

	// The recorded values are the StateToValue mapping (1.0 for Running, 0.0 for Off).
	auto points = service.QuerySeries(series.front().key, 0, now + 1, 100);
	BOOST_REQUIRE_EQUAL(points.size(), 2u);
	BOOST_CHECK_EQUAL(points.front().value, 1.0);
	BOOST_CHECK_EQUAL(points.back().value, 0.0);
}

//=============================================================================
// Heartbeat -> SampleCurrentState reads current DataHub state; a chlorinator
// carrying a DutyCycleTrait records the swg/percent series.
//=============================================================================

BOOST_AUTO_TEST_CASE(Heartbeat_SamplesChlorinatorDutyCycle)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	std::int64_t now = 8'000;
	service.SetClock([&now] { return now; });
	service.Start();

	using namespace Kernel::AuxillaryTraitsTypes;
	auto data_hub = Find<Kernel::DataHub>();

	auto chlor = std::make_shared<Kernel::AuxillaryDevice>();
	chlor->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
	chlor->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
	chlor->AuxillaryTraits.Set(DutyCycleTrait{}, static_cast<std::uint8_t>(55));
	data_hub->Devices.Add(chlor);

	// The heartbeat bypasses the throttle and records the SWG duty cycle.
	service.Heartbeat();

	auto series = service.ListSeries();
	bool saw_swg = false;
	for (const auto& s : series)
	{
		if (s.key == "swg/percent")
		{
			saw_swg = true;
			BOOST_CHECK_EQUAL(s.unit, "%");
		}
	}
	BOOST_CHECK(saw_swg);
}

//=============================================================================
// Schema migration: Start() opening a pre-existing on-disk database created
// before the `label` column existed runs MigrateSchema, which probes
// table_info(series) and ALTERs in the missing column. An in-memory database
// cannot exercise this (it is recreated fresh each connection), so use a real
// temp file: seed the legacy schema through a raw SqliteDb, close it, then let
// the service reopen and migrate it.
//=============================================================================

BOOST_AUTO_TEST_CASE(Start_MigratesLegacyDbWithoutLabelColumn)
{
	// Unique temp path so the test never collides with a stale file.
	const auto db_path = (std::filesystem::temp_directory_path() /
		std::filesystem::path{ std::format("aqualink_history_migrate_{}.db",
			static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) }).string();

	std::filesystem::remove(db_path);   // ensure a clean start

	// Seed the pre-`label` schema through a raw connection, then close it so the
	// service can reopen the same file.
	{
		History::SqliteDb legacy(db_path);
		legacy.Exec(
			"CREATE TABLE series ("
			"  id INTEGER PRIMARY KEY,"
			"  key TEXT UNIQUE NOT NULL,"
			"  unit TEXT);"
			"CREATE TABLE samples ("
			"  series_id INTEGER NOT NULL REFERENCES series(id),"
			"  ts INTEGER NOT NULL,"
			"  value REAL NOT NULL);");
	}

	{
		boost::asio::io_context io;
		auto settings = MemorySettings();
		settings.db_path = db_path;
		History::HistoryService service(io, *this, settings);

		std::int64_t now = 100;
		service.SetClock([&now] { return now; });

		// Start() -> MigrateSchema() finds no `label` column and ALTERs it in.
		BOOST_CHECK_NO_THROW(service.Start());

		// Proof the column now exists: a labelled device series records and the
		// label round-trips through the migrated table.
		service.RecordDeviceState("device/uuid-migrated", "Pool Light", 1.0);

		auto series = service.ListSeries();
		BOOST_REQUIRE_EQUAL(series.size(), 1u);
		BOOST_CHECK_EQUAL(series.front().key, "device/uuid-migrated");
		BOOST_CHECK_EQUAL(series.front().label, "Pool Light");
		BOOST_CHECK_EQUAL(series.front().count, 1);

		service.Stop();
	}

	std::filesystem::remove(db_path);   // cleanup
}

BOOST_AUTO_TEST_SUITE_END()
