#pragma once
#include "types.h"
#include <cinttypes>
#include <mutex>

enum LOGLEVEL
{
  LOGLEVEL_NONE = 0,    // Silences all log traffic
  LOGLEVEL_ERROR = 1,   // "ErrorPrint"
  LOGLEVEL_WARNING = 2, // "WarningPrint"
  LOGLEVEL_PERF = 3,    // "PerfPrint"
  LOGLEVEL_INFO = 4,    // "InfoPrint"
  LOGLEVEL_VERBOSE = 5, // "VerbosePrint"
  LOGLEVEL_DEV = 6,     // "DevPrint"
  LOGLEVEL_PROFILE = 7, // "ProfilePrint"
  LOGLEVEL_DEBUG = 8,   // "DebugPrint"
  LOGLEVEL_TRACE = 9,   // "TracePrint"
  LOGLEVEL_COUNT = 10
};

namespace Log {
// log message callback type
using CallbackFunctionType = void (*)(void* pUserParam, const char* channelName, const char* functionName,
                                      LOGLEVEL level, const char* message);

// registers a log callback
void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// unregisters a log callback
void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// adds a standard console output
bool IsConsoleOutputEnabled();
void SetConsoleOutputParams(bool enabled, const char* channelFilter = nullptr, LOGLEVEL levelFilter = LOGLEVEL_TRACE);

// adds a debug console output [win32/android only]
bool IsDebugOutputEnabled();
void SetDebugOutputParams(bool enabled, const char* channelFilter = nullptr, LOGLEVEL levelFilter = LOGLEVEL_TRACE);

// adds a file output
void SetFileOutputParams(bool enabled, const char* filename, bool timestamps = true,
                         const char* channelFilter = nullptr, LOGLEVEL levelFilter = LOGLEVEL_TRACE);

// Sets global filtering level, messages below this level won't be sent to any of the logging sinks.
void SetFilterLevel(LOGLEVEL level);

// writes a message to the log
void Write(const char* channelName, const char* functionName, LOGLEVEL level, const char* message);
void Writef(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, ...) printflike(4, 5);
void Writev(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, va_list ap);
} // namespace Log

// log wrappers
#define Log_SetChannel(ChannelName) static const char* ___LogChannel___ = #ChannelName;
#define Log_ErrorPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_ERROR, msg)
#define Log_ErrorPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_ERROR, __VA_ARGS__)
#define Log_WarningPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_WARNING, msg)
#define Log_WarningPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_WARNING, __VA_ARGS__)
#define Log_PerfPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_PERF, msg)
#define Log_PerfPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_PERF, __VA_ARGS__)
#define Log_InfoPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_INFO, msg)
#define Log_InfoPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_INFO, __VA_ARGS__)
#define Log_VerbosePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_VERBOSE, msg)
#define Log_VerbosePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_VERBOSE, __VA_ARGS__)
#define Log_DevPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_DEV, msg)
#define Log_DevPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_DEV, __VA_ARGS__)
#define Log_ProfilePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_PROFILE, msg)
#define Log_ProfilePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_PROFILE, __VA_ARGS__)

#ifdef _DEBUG
#define Log_DebugPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_DEBUG, msg)
#define Log_DebugPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_DEBUG, __VA_ARGS__)
#define Log_TracePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_TRACE, msg)
#define Log_TracePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_TRACE, __VA_ARGS__)
#else
#define Log_DebugPrint(msg)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define Log_DebugPrintf(...)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define Log_TracePrint(msg)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define Log_TracePrintf(...)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#endif
