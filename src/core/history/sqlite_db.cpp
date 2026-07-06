#include <format>
#include <stdexcept>

#include <sqlite3.h>

#include "history/sqlite_db.h"
#include "exceptions/exception_history_databaseerror.h"

namespace AqualinkAutomate::History
{
	namespace
	{
		[[noreturn]] void Throw(sqlite3* db, const std::string& context)
		{
			const char* msg = (db != nullptr) ? sqlite3_errmsg(db) : "no database handle";
			throw Exceptions::History_DatabaseError(std::format("{}: {}", context, msg ? msg : "unknown error"));
		}
	}

	SqliteDb::SqliteDb(const std::string& path)
	{
		if (sqlite3_open(path.c_str(), &m_Db) != SQLITE_OK)
		{
			// sqlite3_open may still allocate a handle on failure; capture the
			// message then close it.
			const std::string message = (m_Db != nullptr) ? sqlite3_errmsg(m_Db) : "out of memory";
			if (m_Db != nullptr) { sqlite3_close(m_Db); m_Db = nullptr; }
			throw Exceptions::History_DatabaseError(std::format("Failed to open SQLite database '{}': {}", path, message));
		}
	}

	SqliteDb::~SqliteDb()
	{
		if (m_Db != nullptr)
		{
			sqlite3_close(m_Db);
			m_Db = nullptr;
		}
	}

	void SqliteDb::Exec(const std::string& sql)
	{
		char* err = nullptr;
		if (sqlite3_exec(m_Db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
		{
			const std::string message = (err != nullptr) ? err : "unknown error";
			sqlite3_free(err);
			throw Exceptions::History_DatabaseError(std::format("SQLite exec failed: {}", message));
		}
	}

	std::int64_t SqliteDb::LastInsertRowId() const noexcept
	{
		return static_cast<std::int64_t>(sqlite3_last_insert_rowid(m_Db));
	}

	SqliteStmt::SqliteStmt(SqliteDb& db, const std::string& sql) :
		m_Db(db)
	{
		if (sqlite3_prepare_v2(m_Db.Handle(), sql.c_str(), -1, &m_Stmt, nullptr) != SQLITE_OK)
		{
			Throw(m_Db.Handle(), std::format("Failed to prepare statement '{}'", sql));
		}
	}

	SqliteStmt::~SqliteStmt()
	{
		if (m_Stmt != nullptr)
		{
			sqlite3_finalize(m_Stmt);
			m_Stmt = nullptr;
		}
	}

	void SqliteStmt::Bind(int index, std::int64_t value)
	{
		if (sqlite3_bind_int64(m_Stmt, index, value) != SQLITE_OK)
		{
			Throw(m_Db.Handle(), "Failed to bind int64");
		}
	}

	void SqliteStmt::Bind(int index, double value)
	{
		if (sqlite3_bind_double(m_Stmt, index, value) != SQLITE_OK)
		{
			Throw(m_Db.Handle(), "Failed to bind double");
		}
	}

	void SqliteStmt::Bind(int index, const std::string& value)
	{
		// SQLITE_TRANSIENT: SQLite copies the string, so the caller's buffer need
		// not outlive the bind.
		if (sqlite3_bind_text(m_Stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
		{
			Throw(m_Db.Handle(), "Failed to bind text");
		}
	}

	bool SqliteStmt::Step()
	{
		const int rc = sqlite3_step(m_Stmt);
		if (rc == SQLITE_ROW) { return true; }
		if (rc == SQLITE_DONE) { return false; }
		Throw(m_Db.Handle(), "Statement step failed");
	}

	void SqliteStmt::Reset()
	{
		sqlite3_reset(m_Stmt);
		sqlite3_clear_bindings(m_Stmt);
	}

	std::int64_t SqliteStmt::ColumnInt64(int column)
	{
		return static_cast<std::int64_t>(sqlite3_column_int64(m_Stmt, column));
	}

	double SqliteStmt::ColumnDouble(int column)
	{
		return sqlite3_column_double(m_Stmt, column);
	}

	std::string SqliteStmt::ColumnText(int column)
	{
		const unsigned char* text = sqlite3_column_text(m_Stmt, column);
		if (text == nullptr) { return {}; }
		return std::string(reinterpret_cast<const char*>(text));
	}

	SqliteTransaction::SqliteTransaction(SqliteDb& db) :
		m_Db(db)
	{
		m_Db.Exec("BEGIN");
	}

	SqliteTransaction::~SqliteTransaction()
	{
		if (!m_Committed)
		{
			// Best-effort rollback; never throw from a destructor.
			char* err = nullptr;
			sqlite3_exec(m_Db.Handle(), "ROLLBACK", nullptr, nullptr, &err);
			if (err != nullptr) { sqlite3_free(err); }
		}
	}

	void SqliteTransaction::Commit()
	{
		m_Db.Exec("COMMIT");
		m_Committed = true;
	}

}
// namespace AqualinkAutomate::History
