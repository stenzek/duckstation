// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine);
[[noreturn]] void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine);

#define Assert(expr)                                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Assertion failed: '" #expr "'", __FUNCTION__, __FILE__, __LINE__);                             \
  } while (0)
#define AssertMsg(expr, msg)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Assertion failed: '" msg "'", __FUNCTION__, __FILE__, __LINE__);                               \
  } while (0)

#if defined(_DEBUG) || defined(_DEVEL)
#define DebugAssert(expr)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Debug assertion failed: '" #expr "'", __FUNCTION__, __FILE__, __LINE__);                       \
  } while (0)
#define DebugAssertMsg(expr, msg)                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(expr))                                                                                                       \
      Y_OnAssertFailed("Debug assertion failed: '" msg "'", __FUNCTION__, __FILE__, __LINE__);                         \
  } while (0)
#else
#define DebugAssert(expr)
#define DebugAssertMsg(expr, msg)
#endif

// Panics the application, displaying an error message.
#define Panic(Message) Y_OnPanicReached("Panic triggered: '" Message "'", __FUNCTION__, __FILE__, __LINE__)

// Kills the application, indicating a pure function call that should not have happened.
#define PureCall() Y_OnPanicReached("PureCall encountered", __FUNCTION__, __FILE__, __LINE__)

#if defined(_DEBUG) || defined(_DEVEL)
// Kills the application, indicating that code that was never supposed to be reached has been executed.
#define UnreachableCode() Y_OnPanicReached("Unreachable code reached", __FUNCTION__, __FILE__, __LINE__)
#else
#define UnreachableCode() ASSUME(false)
#endif

// Helper for switch cases.
#define DefaultCaseIsUnreachable()                                                                                     \
  default:                                                                                                             \
    UnreachableCode();                                                                                                 \
    break;
