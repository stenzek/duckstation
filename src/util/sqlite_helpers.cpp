// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sqlite_helpers.h"

#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <mutex>
#include <type_traits>

LOG_CHANNEL(Host);

namespace {
struct Locals
{
  // Dynamic libraries
  DynamicLibrary sqlite_library;
  std::once_flag sqlite_init_flag;
};
} // namespace

static Locals s_locals;

static_assert(std::is_trivially_copyable_v<DynSqlite> && std::is_standard_layout_v<DynSqlite>);
DynSqlite g_dyn_sqlite;

bool DynSqlite::Open(Error* error)
{
  // Because of course friggin linux is different...
#ifdef _WIN32
  static constexpr int lib_major_version = -1;
#else
  static constexpr int lib_major_version = 3;
#endif

  if (s_locals.sqlite_library.IsOpen())
    return true;

  std::call_once(s_locals.sqlite_init_flag, [&error]() {
    Error lerror;
    DynamicLibrary lib;
    if (!lib.Open(DynamicLibrary::GetBundledLibraryPath("sqlite3", lib_major_version).c_str(), &lerror))
    {
      ERROR_LOG("Failed to load sqlite: {}", lerror.GetDescription());
      Error::SetStringFmt(error, "Failed to load sqlite: {}", lerror.GetDescription());
      return;
    }

    // clang-format off
  static const DynamicLibrary::SymbolTable sqlite_symbols[] = {
#define SQLITE_SYMBOL(F) {#F, (void**)&g_dyn_sqlite.F},
    DYN_SQLITE_FUNCTIONS(SQLITE_SYMBOL)
#undef SQLITE_SYMBOL
  };

    if (!lib.ResolveSymbols(sqlite_symbols, std::size(sqlite_symbols), error))
      return;

    s_locals.sqlite_library = std::move(lib);
  });

  return s_locals.sqlite_library.IsOpen();
}

sqlite3* SQLiteHelpers::OpenAndCheckDatabase(const char* const path, Error* const error)
{
  if (!g_dyn_sqlite.Open(error)) [[unlikely]]
    return nullptr;

  sqlite3* db = nullptr;
  int rc = g_dyn_sqlite.sqlite3_open_v2(path, &db,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);

  // Sanity check on the database.
  if (rc == SQLITE_OK)
  {
    const int check_rc =
      g_dyn_sqlite.sqlite3_exec(db, "PRAGMA schema_version;", nullptr, nullptr, nullptr);
    if (check_rc != SQLITE_OK)
    {
      const char* errmsg = g_dyn_sqlite.sqlite3_errmsg(db);
      WARNING_LOG("Database {} failed sanity check, rc={}: {}", Path::GetFileName(path), check_rc, errmsg ? errmsg : "<unknown error>");
      g_dyn_sqlite.sqlite3_close(db);
      rc = SQLITE_NOTADB;
    }
  }

  // If the database is corrupted, then delete it and try again.
  if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB)
  {
    WARNING_LOG("Database {} is corrupted, deleting and trying again.", Path::GetFileName(path));
    if (Error lerror; !FileSystem::DeleteFile(path, &lerror))
    {
      ERROR_LOG("Failed to delete corrupted database {}: {}", Path::GetFileName(path), lerror.GetDescription());
    }
    else
    {
      rc = g_dyn_sqlite.sqlite3_open_v2(path, &db,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    }
  }

  if (rc != SQLITE_OK)
  {
    if (db)
      g_dyn_sqlite.sqlite3_close(db);
    Error::SetStringFmt(error, "sqlite3_open_v2() failed: {}", rc);
    return nullptr;
  }

  return db;
}

void SQLiteHelpers::SetError(Error* const error, sqlite3* const db, std::string_view prefix)
{
  const char* errmsg = g_dyn_sqlite.sqlite3_errmsg(db);
  if (!prefix.empty())
    Error::SetStringFmt(error, "{}{}", prefix, errmsg ? errmsg : "<unknown error>");
  else if (errmsg)
    Error::SetStringView(error, errmsg ? errmsg : "<unknown error>");   
}

bool SQLiteHelpers::Execute(sqlite3* const db, const char* sql, Error* const error /*= nullptr*/)
{
  char* errmsg = nullptr;
  if (g_dyn_sqlite.sqlite3_exec(db, sql, nullptr, nullptr, &errmsg) == SQLITE_OK)
    return true;

  Error::SetStringFmt(error, "Failed to execute SQL: {}", errmsg ? errmsg : "<unknown error>");
  g_dyn_sqlite.sqlite3_free(errmsg);
  return false;
}

bool SQLiteHelpers::BeginTransaction(sqlite3* const db, Error* const error /*= nullptr*/)
{
  return Execute(db, "BEGIN TRANSACTION;", error);
}

bool SQLiteHelpers::CommitTransaction(sqlite3* const db, Error* const error /*= nullptr*/)
{
  return Execute(db, "COMMIT;", error);
}

void SQLiteHelpers::RollbackTransaction(sqlite3* const db)
{
  Execute(db, "ROLLBACK;");
}

SQLitePreparedStatement::SQLitePreparedStatement(SQLitePreparedStatement&& other)
  : m_stmt(std::exchange(other.m_stmt, nullptr))
{
}

bool SQLitePreparedStatement::Prepare(sqlite3* const db, const char* sql, Error* const error /*= nullptr*/)
{
  if (m_stmt)
  {
    g_dyn_sqlite.sqlite3_finalize(m_stmt);
    m_stmt = nullptr;
  }

  if (g_dyn_sqlite.sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK)
  {
    if (error)
      Error::SetStringFmt(error, "Failed to prepare statement: {}", g_dyn_sqlite.sqlite3_errmsg(db));
    return false;
  }

  return true;
}

void SQLitePreparedStatement::Destroy()
{
  if (m_stmt)
  {
    g_dyn_sqlite.sqlite3_finalize(m_stmt);
    m_stmt = nullptr;
  }
}

bool SQLitePreparedStatement::Execute(sqlite3* const db, Error* const error /* = nullptr */)
{
  if (g_dyn_sqlite.sqlite3_step(m_stmt) != SQLITE_DONE) [[unlikely]]
  {
    SQLiteHelpers::SetError(error, db, "Failed to execute statement: ");
    return false;
  }

  return true;
}

SQLitePreparedStatement& SQLitePreparedStatement::operator=(SQLitePreparedStatement&& other)
{
  if (this != &other)
  {
    if (m_stmt)
      g_dyn_sqlite.sqlite3_finalize(m_stmt);

    m_stmt = std::exchange(other.m_stmt, nullptr);
  }

  return *this;
}
