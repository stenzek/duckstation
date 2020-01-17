#pragma once

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine);
void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine);

#define Assert(expr)                                                                                                   \
  if (!(expr))                                                                                                         \
  {                                                                                                                    \
    Y_OnAssertFailed("Assertion failed: '" #expr "'", __FUNCTION__, __FILE__, __LINE__);                               \
  }
#define AssertMsg(expr, msg)                                                                                           \
  if (!(expr))                                                                                                         \
  {                                                                                                                    \
    Y_OnAssertFailed("Assertion failed: '" msg "'", __FUNCTION__, __FILE__, __LINE__);                                 \
  }

#ifdef _DEBUG
#define DebugAssert(expr)                                                                                              \
  if (!(expr))                                                                                                         \
  {                                                                                                                    \
    Y_OnAssertFailed("Debug assertion failed: '" #expr "'", __FUNCTION__, __FILE__, __LINE__);                         \
  }
#define DebugAssertMsg(expr, msg)                                                                                      \
  if (!(expr))                                                                                                         \
  {                                                                                                                    \
    Y_OnAssertFailed("Debug assertion failed: '" msg "'", __FUNCTION__, __FILE__, __LINE__);                           \
  }
#define DebugUnreachableCode() Y_OnPanicReached("Unreachable code reached", __FUNCTION__, __FILE__, __LINE__)
#else
#define DebugAssert(expr)
#define DebugAssertMsg(expr, msg)
#define DebugUnreachableCode()
#endif

// Panics the application, displaying an error message.
#define Panic(Message) Y_OnPanicReached("Panic triggered: '" Message "'", __FUNCTION__, __FILE__, __LINE__)

// Kills the application, indicating a pure function call that should not have happened.
#define PureCall() Y_OnPanicReached("PureCall encountered", __FUNCTION__, __FILE__, __LINE__)

// Kills the application, indicating that code that was never supposed to be reached has been executed.
#define UnreachableCode() Y_OnPanicReached("Unreachable code reached", __FUNCTION__, __FILE__, __LINE__)

// Helper for switch cases.
#define DefaultCaseIsUnreachable()                                                                                     \
  default:                                                                                                             \
    UnreachableCode();                                                                                                 \
    break;
