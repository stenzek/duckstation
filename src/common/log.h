// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "types.h"

#include "fmt/core.h"

#include <cinttypes>
#include <cstdarg>
#include <mutex>
#include <string_view>

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
                                      LOGLEVEL level, std::string_view message);

// registers a log callback
void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// unregisters a log callback
void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// returns the time in seconds since the start of the process
float GetCurrentMessageTime();

// adds a standard console output
bool IsConsoleOutputEnabled();
void SetConsoleOutputParams(bool enabled, bool timestamps = true);

// adds a debug console output [win32/android only]
bool IsDebugOutputEnabled();
void SetDebugOutputParams(bool enabled);

// adds a file output
void SetFileOutputParams(bool enabled, const char* filename, bool timestamps = true);

// Returns the current global filtering level.
LOGLEVEL GetLogLevel();

// Returns true if log messages for the specified log level/filter would not be filtered (and visible).
bool IsLogVisible(LOGLEVEL level, const char* channelName);

// Sets global filtering level, messages below this level won't be sent to any of the logging sinks.
void SetLogLevel(LOGLEVEL level);

// Sets global filter, any messages from these channels won't be sent to any of the logging sinks.
void SetLogFilter(std::string_view filter);

// writes a message to the log
void Write(const char* channelName, const char* functionName, LOGLEVEL level, std::string_view message);
void Writef(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, ...)
  printflike(4, 5);
void Writev(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, va_list ap);
void WriteFmtArgs(const char* channelName, const char* functionName, LOGLEVEL level, fmt::string_view fmt,
                  fmt::format_args args);

template<typename... T>
ALWAYS_INLINE static void WriteFmt(const char* channelName, const char* functionName, LOGLEVEL level,
                                   fmt::format_string<T...> fmt, T&&... args)
{
  if (level <= GetLogLevel())
    return WriteFmtArgs(channelName, functionName, level, fmt, fmt::make_format_args(args...));
}
} // namespace Log

// log wrappers
#define Log_SetChannel(ChannelName) [[maybe_unused]] static const char* ___LogChannel___ = #ChannelName;
#define Log_ErrorPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_ERROR, msg)
#define Log_ErrorPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_ERROR, __VA_ARGS__)
#define Log_ErrorFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_ERROR, __VA_ARGS__)
#define Log_WarningPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_WARNING, msg)
#define Log_WarningPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_WARNING, __VA_ARGS__)
#define Log_WarningFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_WARNING, __VA_ARGS__)
#define Log_PerfPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_PERF, msg)
#define Log_PerfPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_PERF, __VA_ARGS__)
#define Log_PerfFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_PERF, __VA_ARGS__)
#define Log_InfoPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_INFO, msg)
#define Log_InfoPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_INFO, __VA_ARGS__)
#define Log_InfoFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_INFO, __VA_ARGS__)
#define Log_VerbosePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_VERBOSE, msg)
#define Log_VerbosePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_VERBOSE, __VA_ARGS__)
#define Log_VerboseFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_VERBOSE, __VA_ARGS__)
#define Log_DevPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_DEV, msg)
#define Log_DevPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_DEV, __VA_ARGS__)
#define Log_DevFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_DEV, __VA_ARGS__)
#define Log_ProfilePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_PROFILE, msg)
#define Log_ProfilePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_PROFILE, __VA_ARGS__)
#define Log_ProfileFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_PROFILE, __VA_ARGS__)

#define Log_ErrorVisible() Log::IsLogVisible(LOGLEVEL_ERROR, ___LogChannel___)
#define Log_WarningVisible() Log::IsLogVisible(LOGLEVEL_WARNING, ___LogChannel___)
#define Log_PerfVisible() Log::IsLogVisible(LOGLEVEL_PERF, ___LogChannel___)
#define Log_InfoVisible() Log::IsLogVisible(LOGLEVEL_INFO, ___LogChannel___)
#define Log_VerboseVisible() Log::IsLogVisible(LOGLEVEL_VERBOSE, ___LogChannel___)
#define Log_DevVisible() Log::IsLogVisible(LOGLEVEL_DEV, ___LogChannel___)
#define Log_ProfileVisible() Log::IsLogVisible(LOGLEVEL_PROFILE, ___LogChannel___)

#ifdef _DEBUG
#define Log_DebugPrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_DEBUG, msg)
#define Log_DebugPrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_DEBUG, __VA_ARGS__)
#define Log_DebugFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_DEBUG, __VA_ARGS__)
#define Log_TracePrint(msg) Log::Write(___LogChannel___, __func__, LOGLEVEL_TRACE, msg)
#define Log_TracePrintf(...) Log::Writef(___LogChannel___, __func__, LOGLEVEL_TRACE, __VA_ARGS__)
#define Log_TraceFmt(...) Log::WriteFmt(___LogChannel___, __func__, LOGLEVEL_TRACE, __VA_ARGS__)

#define Log_DebugVisible() Log::IsLogVisible(LOGLEVEL_DEBUG, ___LogChannel___)
#define Log_TraceVisible() Log::IsLogVisible(LOGLEVEL_TRACE, ___LogChannel___)
#else
#define Log_DebugPrint(msg)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define Log_DebugPrintf(...)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define Log_DebugFmt(...)                                                                                              \
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
#define Log_TraceFmt(...)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)

#define Log_DebugVisible() false
#define Log_TraceVisible() false
#endif
