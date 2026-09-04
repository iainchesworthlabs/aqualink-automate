#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>

#include "exceptions/exception_history_databaseerror.h"
#include "history/history_service.h"
#include "history/sqlite_db.h"
#include "kernel/data_hub.h"
#include "kernel/hub_events/data_hub_config_event_circulation.h"
#include "kernel/preferences_hub.h"
#include "options/options_history_options.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

//=============================================================================
// Error and lifecycle arms of the history subsystem that the happy-path suite
// (test_history_service.cpp) does not reach:
//
//   * sqlite_db.cpp -- every Throw() site: a failed Exec, a failed prepare, and
//     out-of-range parameter binds; plus LastInsertRowId and a NULL text column.
//   * history_service.cpp -- calls made before Start()/after Stop(), a second
//     Start(), the cancelled-timer completions, the state-string mapping, an
//     unrelated config event, and (the big one) the storage-fault guards: every
//     Record*/Flush/PurgeOld entry point must SWALLOW a storage error rather
//     than let it escape into the single-threaded main loop.
//=============================================================================

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

	// A unique temp file path (the caller owns removal).
	std::string TempDbPath(std::string_view tag)
	{
		static std::uint64_t counter{ 0 };
		const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

		const auto path = std::filesystem::temp_directory_path() /
			std::filesystem::path{ std::format("aqualink_history_{}_{}_{}.db", tag, stamp, counter++) };

		std::error_code ec;
		std::filesystem::remove(path, ec);

		return path.string();
	}

}
// unnamed namespace

//=============================================================================
// SqliteDb / SqliteStmt error surface
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_SqliteDbBranches)

BOOST_AUTO_TEST_CASE(SqliteDbBranches_ExecInvalidSqlThrows)
{
	History::SqliteDb db(":memory:");

	BOOST_CHECK_THROW(db.Exec("THIS IS NOT SQL;"), Exceptions::History_DatabaseError);

	// A statement against a missing table fails the same way.
	BOOST_CHECK_THROW(db.Exec("INSERT INTO no_such_table(v) VALUES(1);"), Exceptions::History_DatabaseError);

	// ...and the connection is still usable afterwards.
	BOOST_CHECK_NO_THROW(db.Exec("CREATE TABLE t(v INTEGER);"));
}

BOOST_AUTO_TEST_CASE(SqliteDbBranches_PrepareInvalidStatementThrows)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(v INTEGER);");

	BOOST_CHECK_THROW(History::SqliteStmt bad(db, "SELECT * FROM missing_table"), Exceptions::History_DatabaseError);
	BOOST_CHECK_THROW(History::SqliteStmt worse(db, "SELECT SELECT SELECT"), Exceptions::History_DatabaseError);
}

// SQLite parameter indices are 1-based; anything outside the statement's
// parameter count is a bind error that must surface as a database exception
// rather than being silently ignored.
BOOST_AUTO_TEST_CASE(SqliteDbBranches_OutOfRangeBindThrows)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(a INTEGER, b REAL, c TEXT);");

	History::SqliteStmt insert(db, "INSERT INTO t(a, b, c) VALUES(?, ?, ?)");

	BOOST_CHECK_THROW(insert.Bind(9, static_cast<std::int64_t>(1)), Exceptions::History_DatabaseError);
	BOOST_CHECK_THROW(insert.Bind(9, 1.5), Exceptions::History_DatabaseError);
	BOOST_CHECK_THROW(insert.Bind(9, std::string{ "x" }), Exceptions::History_DatabaseError);

	// The in-range binds still work.
	BOOST_CHECK_NO_THROW(insert.Bind(1, static_cast<std::int64_t>(7)));
	BOOST_CHECK_NO_THROW(insert.Bind(2, 2.5));
	BOOST_CHECK_NO_THROW(insert.Bind(3, std::string{ "ok" }));
	BOOST_CHECK(!insert.Step());
}

BOOST_AUTO_TEST_CASE(SqliteDbBranches_LastInsertRowIdTracksInserts)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER);");

	db.Exec("INSERT INTO t(v) VALUES(10);");
	const std::int64_t first = db.LastInsertRowId();
	BOOST_CHECK_EQUAL(first, 1);

	db.Exec("INSERT INTO t(v) VALUES(20);");
	BOOST_CHECK_EQUAL(db.LastInsertRowId(), first + 1);
}

// A NULL text column reads back as an empty string, not a dereferenced null.
BOOST_AUTO_TEST_CASE(SqliteDbBranches_NullTextColumnReadsEmpty)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(k TEXT, v TEXT);");
	db.Exec("INSERT INTO t(k, v) VALUES('present', NULL);");

	History::SqliteStmt select(db, "SELECT k, v FROM t");
	BOOST_REQUIRE(select.Step());
	BOOST_CHECK_EQUAL(select.ColumnText(0), "present");
	BOOST_CHECK_EQUAL(select.ColumnText(1), "");
}

BOOST_AUTO_TEST_CASE(SqliteDbBranches_TransactionCommitPersists)
{
	History::SqliteDb db(":memory:");
	db.Exec("CREATE TABLE t(v INTEGER);");

	{
		History::SqliteTransaction txn(db);
		db.Exec("INSERT INTO t(v) VALUES(1);");
		db.Exec("INSERT INTO t(v) VALUES(2);");
		txn.Commit();
	}

	History::SqliteStmt count(db, "SELECT COUNT(*) FROM t");
	BOOST_REQUIRE(count.Step());
	BOOST_CHECK_EQUAL(count.ColumnInt64(0), 2);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// HistoryService lifecycle / guard arms
//=============================================================================

BOOST_FIXTURE_TEST_SUITE(TestSuite_HistoryServiceBranches, Test::HubLocatorInjector)

// Every public write and maintenance entry point is a no-op while the service
// is not running: no database handle exists, so they must return immediately
// rather than dereference it.
BOOST_AUTO_TEST_CASE(HistoryBranches_WriteApiBeforeStartIsInert)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());
	service.SetClock([] { return static_cast<std::int64_t>(500); });

	BOOST_CHECK_NO_THROW(service.RecordNumeric("temp/pool", "C", 20.0));
	BOOST_CHECK_NO_THROW(service.RecordState("device/pump/state", 1.0));
	BOOST_CHECK_NO_THROW(service.RecordDeviceState("device/uuid", "Pump", 1.0));
	BOOST_CHECK_NO_THROW(service.Flush());
	BOOST_CHECK_NO_THROW(service.PurgeOld());
	BOOST_CHECK_NO_THROW(service.Heartbeat());

	// Nothing was recorded, and the read API still answers empty.
	BOOST_CHECK(service.ListSeries().empty());
	BOOST_CHECK(!service.SeriesExists("temp/pool"));
}

// ...and the same holds after Stop(): the handle is released, so late writes
// (e.g. a signal slot that has not been disconnected yet) are dropped.
BOOST_AUTO_TEST_CASE(HistoryBranches_WriteApiAfterStopIsInert)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 1'000;
	service.SetClock([&now] { return now; });
	service.Start();
	service.Stop();

	BOOST_CHECK_NO_THROW(service.RecordNumeric("temp/pool", "C", 20.0));
	BOOST_CHECK_NO_THROW(service.RecordState("device/pump/state", 1.0));
	BOOST_CHECK_NO_THROW(service.RecordDeviceState("device/uuid", "Pump", 1.0));

	BOOST_CHECK(service.ListSeries().empty());
}

BOOST_AUTO_TEST_CASE(HistoryBranches_StartAndStopAreIdempotent)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 2'000;
	service.SetClock([&now] { return now; });

	service.Start();
	service.RecordNumeric("temp/pool", "C", 20.0, true);

	// A second Start() must not reopen the database and lose the buffered sample.
	BOOST_CHECK_NO_THROW(service.Start());

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().count, 1);

	BOOST_CHECK_NO_THROW(service.Stop());
	BOOST_CHECK_NO_THROW(service.Stop());
}

// Stop() cancels the flush / heartbeat / purge timers; their completion
// handlers then run with operation_aborted and must do nothing at all.
BOOST_AUTO_TEST_CASE(HistoryBranches_CancelledTimerHandlersDoNothing)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 3'000;
	service.SetClock([&now] { return now; });
	service.Start();
	service.Stop();

	// Drain the cancelled timer completions - none of them may record, purge or
	// re-arm anything now that the service has stopped.
	BOOST_CHECK_NO_THROW(io.poll());

	BOOST_CHECK(service.ListSeries().empty());
}

// An unopenable database path is a fatal Start() error (unlike a storage fault
// mid-run, which is swallowed).
BOOST_AUTO_TEST_CASE(HistoryBranches_StartWithUnopenableDbThrows)
{
	boost::asio::io_context io;

	auto settings = MemorySettings();
	settings.db_path = (std::filesystem::temp_directory_path() / "aqualink-no-such-dir-xyz" / "nested" / "history.db").string();

	History::HistoryService service(io, *this, settings);

	BOOST_CHECK_THROW(service.Start(), Exceptions::History_DatabaseError);

	// The failed Start left the service stopped, so the write API stays inert.
	BOOST_CHECK_NO_THROW(service.RecordNumeric("temp/pool", "C", 20.0));
}

//=============================================================================
// Config-event mapping
//=============================================================================

// StateToValue treats the whole "device is doing something" vocabulary as 1.0
// and everything else as 0.0, case-insensitively.
BOOST_AUTO_TEST_CASE(HistoryBranches_ButtonStatusVocabularyMapsToOneOrZero)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 4'000;
	service.SetClock([&now] { return now; });
	service.Start();

	auto data_hub = Find<Kernel::DataHub>();

	boost::uuids::string_generator gen;
	const auto button = gen("11111111-2222-3333-4444-555555555555");

	// Each of these is a DIFFERENT (status, label) pair so the DataHub's
	// de-duplication does not swallow it.
	data_hub->EmitButtonStateChange(button, "Running", "Heater");
	now += 10;
	data_hub->EmitButtonStateChange(button, "ENABLED", "Heater");
	now += 10;
	data_hub->EmitButtonStateChange(button, "Heating", "Heater");
	now += 10;
	data_hub->EmitButtonStateChange(button, "Cooling", "Heater");
	now += 10;
	data_hub->EmitButtonStateChange(button, "Off", "Heater");

	auto series = service.ListSeries();
	BOOST_REQUIRE_EQUAL(series.size(), 1u);
	BOOST_CHECK_EQUAL(series.front().count, 5);

	auto points = service.QuerySeries(series.front().key, 3'900, now + 1, 1000);
	BOOST_REQUIRE_EQUAL(points.size(), 5u);
	BOOST_CHECK_EQUAL(points[0].value, 1.0);   // Running
	BOOST_CHECK_EQUAL(points[1].value, 1.0);   // ENABLED (case-insensitive)
	BOOST_CHECK_EQUAL(points[2].value, 1.0);   // Heating
	BOOST_CHECK_EQUAL(points[3].value, 0.0);   // Cooling is not an "active" state
	BOOST_CHECK_EQUAL(points[4].value, 0.0);   // Off
}

// A config event the recorder does not care about (circulation) falls through
// every branch and records nothing - it must not be mistaken for one of the
// sampled event kinds.  A null event must likewise be tolerated: the slot is
// wired to a public signal that any producer can fire.
BOOST_AUTO_TEST_CASE(HistoryBranches_UnrelatedAndNullConfigEventsRecordNothing)
{
	boost::asio::io_context io;
	History::HistoryService service(io, *this, MemorySettings());

	std::int64_t now = 5'000;
	service.SetClock([&now] { return now; });
	service.Start();

	auto data_hub = Find<Kernel::DataHub>();

	auto circulation = std::make_shared<Kernel::DataHub_ConfigEvent_Circulation>();
	circulation->Mode(Kernel::CirculationModes::Spa);
	circulation->AddBody(Kernel::BodyOfWaterIds::Pool, true);

	BOOST_CHECK_NO_THROW(data_hub->ConfigUpdateSignal(circulation));
	BOOST_CHECK_NO_THROW(data_hub->ConfigUpdateSignal(std::shared_ptr<Kernel::DataHub_ConfigEvent>{}));

	BOOST_CHECK(service.ListSeries().empty());
}

//=============================================================================
// Storage faults are contained
//=============================================================================

// Every write/maintenance path is a boundary: it is reached from asio timer
// completions and from a DataHub signal slot on the single-threaded main loop,
// so a storage error must be logged and swallowed, NEVER propagated. Simulate
// a real fault by dropping the tables out from under the live connection
// (a second connection to the same file), then drive each entry point.
BOOST_AUTO_TEST_CASE(HistoryBranches_StorageFaultIsSwallowedByEveryWritePath)
{
	const std::string db_path = TempDbPath("fault");

	{
		boost::asio::io_context io;

		auto settings = MemorySettings();
		settings.db_path = db_path;

		History::HistoryService service(io, *this, settings);

		std::int64_t now = 6'000;
		service.SetClock([&now] { return now; });
		service.Start();

		Find<Kernel::PreferencesHub>()->HistoryRetentionDays = 90;

		// Buffer a sample so Flush has real work to attempt after the fault.
		service.RecordNumeric("temp/pool", "C", 20.0, true);

		// Pull the schema out from under the running service.
		{
			History::SqliteDb saboteur(db_path);
			saboteur.Exec("DROP TABLE samples;");
			saboteur.Exec("DROP TABLE series;");
		}

		// The buffered flush now fails; the buffer is retained and nothing escapes.
		BOOST_CHECK_NO_THROW(service.Flush());

		// New series cannot be created either - each entry point swallows it.
		BOOST_CHECK_NO_THROW(service.RecordNumeric("temp/spa", "C", 21.0, true));
		BOOST_CHECK_NO_THROW(service.RecordState("device/pump/state", 1.0));
		BOOST_CHECK_NO_THROW(service.RecordDeviceState("device/uuid-fault", "Pool Light", 1.0));

		// ...as does the retention sweep.
		BOOST_CHECK_NO_THROW(service.PurgeOld());

		// And the shutdown flush, which retries the same failing write.
		BOOST_CHECK_NO_THROW(service.Stop());
	}

	std::error_code ec;
	std::filesystem::remove(db_path, ec);
	std::filesystem::remove(db_path + "-wal", ec);
	std::filesystem::remove(db_path + "-shm", ec);
}

BOOST_AUTO_TEST_SUITE_END()
