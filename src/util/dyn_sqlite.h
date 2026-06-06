// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <sqlite3.h>

class Error;

#define DYN_SQLITE_FUNCTIONS(X)                                                                                        \
  X(sqlite3_open_v2)                                                                                                   \
  X(sqlite3_close)                                                                                                     \
  X(sqlite3_errmsg)                                                                                                    \
  X(sqlite3_free)                                                                                                      \
  X(sqlite3_prepare_v2)                                                                                                \
  X(sqlite3_finalize)                                                                                                  \
  X(sqlite3_step)                                                                                                      \
  X(sqlite3_reset)                                                                                                     \
  X(sqlite3_exec)                                                                                                      \
  X(sqlite3_bind_blob)                                                                                                 \
  X(sqlite3_bind_int)                                                                                                  \
  X(sqlite3_bind_text)                                                                                                 \
  X(sqlite3_column_blob)                                                                                               \
  X(sqlite3_column_bytes)                                                                                              \
  X(sqlite3_column_text)                                                                                               \
  X(sqlite3_column_int)

struct DynSqlite
{
#define ADD_FUNC(F) decltype(&::F) F;
  DYN_SQLITE_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

  bool Open(Error* error);
};

extern DynSqlite g_dyn_sqlite;
