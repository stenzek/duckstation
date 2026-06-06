// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "dyn_sqlite.h"

class Error;

namespace SQLiteHelpers {

sqlite3* OpenAndCheckDatabase(const char* const path, Error* const error);
void SetError(Error* const error, sqlite3* const db, std::string_view prefix = {});
bool Execute(sqlite3* const db, const char* sql, Error* const error = nullptr);
bool BeginTransaction(sqlite3* const db, Error* const error = nullptr);
bool CommitTransaction(sqlite3* const db, Error* const error = nullptr);
void RollbackTransaction(sqlite3* const db);

} // namespace SQLiteHelpers

class SQLitePreparedStatement
{
public:
  SQLitePreparedStatement() = default;
  ~SQLitePreparedStatement()
  {
    if (m_stmt)
      g_dyn_sqlite.sqlite3_finalize(m_stmt);
  }

  SQLitePreparedStatement(const SQLitePreparedStatement&) = delete;
  SQLitePreparedStatement& operator=(const SQLitePreparedStatement&) = delete;

  SQLitePreparedStatement(SQLitePreparedStatement&& other);
  SQLitePreparedStatement& operator=(SQLitePreparedStatement&& other);

  explicit operator bool() const { return (m_stmt != nullptr); }

  bool Prepare(sqlite3* const db, const char* sql, Error* const error = nullptr);
  void Destroy();

  void BindBlob(int idx, const void* data, int size)
  {
    g_dyn_sqlite.sqlite3_bind_blob(m_stmt, idx, data, size, SQLITE_STATIC);
  }
  void BindBlob(int idx, const std::span<const u8> data) { BindBlob(idx, data.data(), static_cast<int>(data.size())); }
  void BindInt(int idx, int val) { g_dyn_sqlite.sqlite3_bind_int(m_stmt, idx, val); }
  void BindText(int idx, const char* text, int size)
  {
    g_dyn_sqlite.sqlite3_bind_text(m_stmt, idx, text, size, SQLITE_STATIC);
  }
  void BindText(int idx, std::string_view text) { BindText(idx, text.data(), static_cast<int>(text.size())); }

  int Step() { return g_dyn_sqlite.sqlite3_step(m_stmt); }
  void Reset() { g_dyn_sqlite.sqlite3_reset(m_stmt); }
  bool Execute(sqlite3* const db, Error* const error = nullptr);

  const void* ColumnBlob(int idx) const { return g_dyn_sqlite.sqlite3_column_blob(m_stmt, idx); }

  std::span<const u8> ColumnBlobBytes(int idx) const
  {
    const u8* bytes = reinterpret_cast<const u8*>(g_dyn_sqlite.sqlite3_column_blob(m_stmt, idx));
    return bytes ? std::span<const u8>(bytes, g_dyn_sqlite.sqlite3_column_bytes(m_stmt, idx)) : std::span<const u8>();
  }

  int ColumnSizeBytes(int idx) const { return g_dyn_sqlite.sqlite3_column_bytes(m_stmt, idx); }

  int ColumnInt(int idx) const { return g_dyn_sqlite.sqlite3_column_int(m_stmt, idx); }

  std::string_view ColumnText(int idx) const
  {
    const char* text = reinterpret_cast<const char*>(g_dyn_sqlite.sqlite3_column_text(m_stmt, idx));
    const int size = g_dyn_sqlite.sqlite3_column_bytes(m_stmt, idx);
    return text ? std::string_view(text, size) : std::string_view();
  }

  const char* ColumnTextCStr(int idx) const
  {
    return reinterpret_cast<const char*>(g_dyn_sqlite.sqlite3_column_text(m_stmt, idx));
  }

private:
  sqlite3_stmt* m_stmt = nullptr;
};
